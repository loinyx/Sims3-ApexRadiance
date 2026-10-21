// RefPack streams as The Sims 3 writes and reads them: a fast compressor, and the game's decompressor and compressor
// translated from TS3W.exe (see refpack_codec.h and docs/features/performance.md, "Faster Cache Compression").
//
// Part of Apex Radiance. Credits: @loinyx
#include "refpack_codec.h"
#include <emmintrin.h>
#include <intrin.h>
#include <cstring>
#include <vector>

namespace RefPackCodec {
namespace {

constexpr uint32_t kHeads = 1u << kHashBits;
constexpr uint32_t kPrevMask = (1u << kPrevBits) - 1;
constexpr uint32_t kMaxLen = 0x404; // 1028: the longest match one opcode holds

inline uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline uint32_t Hash4(uint32_t v) { return (v * 2654435761u) >> (32 - kHashBits); }

// Length of the common prefix of a and b (the first 4 bytes are known to be equal), at most maxLen
inline uint32_t MatchLen(const uint8_t* a, const uint8_t* b, uint32_t maxLen) {
    uint32_t n = 4;
    while(n+32<=maxLen){
        const unsigned first=static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(a+n)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+n)))))^0xFFFFu;
        if(first){unsigned long z;_BitScanForward(&z,first);return n+z;}
        const unsigned second=static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(a+n+16)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+n+16)))))^0xFFFFu;
        if(second){unsigned long z;_BitScanForward(&z,second);return n+16+z;}
        n+=32;
    }
    while (n + 16 <= maxLen) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + n));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + n));
        const unsigned diff = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(x, y))) ^ 0xFFFFu;
        if (diff) {
            unsigned long z;
            _BitScanForward(&z, diff);
            return n + z;
        }
        n += 16;
    }
    while (n + 4 <= maxLen) {
        const uint32_t x = Load32(a + n) ^ Load32(b + n);
        if (x) {
            unsigned long z;
            _BitScanForward(&z, x);
            return n + (z >> 3);
        }
        n += 4;
    }
    while (n < maxLen && a[n] == b[n]) n++;
    return n;
}

// Opcode bytes for a match (the game's choice, 0x004EB8A1): 2 when offset <= 1024 and length <= 10, 3 when offset <=
// 16384 and length <= 67, else 4. Combinations no opcode can hold get a prohibitive cost.
inline int Cost(uint32_t len, uint32_t off) {
    if (off <= 1024 && len <= 10) return len >= 3 ? 2 : 1000;
    if (off <= 16384 && len <= 67) return len >= 4 ? 3 : 1000;
    if (off <= 131072 && len >= 5 && len <= kMaxLen) return 4;
    return 1000;
}

// Output with an optional capacity (dst == nullptr: count only)
struct Writer {
    uint8_t* dst;
    uint32_t cap; // 0 = unlimited
    uint32_t n = 0;
    bool full = false;
    bool Room(uint32_t k) {
        if (dst && cap && (n > cap || cap - n < k)) full = true;
        return !full;
    }
    void Byte(uint32_t b) {
        if (dst) dst[n] = static_cast<uint8_t>(b);
        n++;
    }
    void Copy(const uint8_t* p, uint32_t k) {
        if (dst && k) std::memcpy(dst + n, p, k);
        n += k;
    }
};

// Literal runs of 4..112 bytes (multiple of 4) while at least 4 are pending; returns the 0..3 left
bool FlushLiterals(Writer& w, const uint8_t* src, uint32_t& from, uint32_t& count) {
    while (count > 3) {
        uint32_t k = count & ~3u;
        if (k > 0x70) k = 0x70;
        if (!w.Room(1 + k)) return false;
        w.Byte(0xE0 + (k >> 2) - 1);
        w.Copy(src + from, k);
        from += k;
        count -= k;
    }
    return true;
}

bool EmitMatch(Writer& w, const uint8_t* lits, uint32_t lit, uint32_t len, uint32_t off, int cost) {
    if (!w.Room(static_cast<uint32_t>(cost) + lit)) return false;
    const uint32_t o = off - 1;
    if (cost == 2) {
        w.Byte(((o >> 3) & 0x60) | ((len - 3) << 2) | lit);
        w.Byte(o & 0xFF);
    } else if (cost == 3) {
        w.Byte(0x80 | (len - 4));
        w.Byte((lit << 6) | (o >> 8));
        w.Byte(o & 0xFF);
    } else {
        w.Byte(0xC0 | ((o >> 12) & 0x10) | (((len - 5) >> 6) & 0x0C) | lit);
        w.Byte((o >> 8) & 0xFF);
        w.Byte(o & 0xFF);
        w.Byte((len - 5) & 0xFF);
    }
    w.Copy(lits, lit);
    return true;
}

// ---- the game's compressor, translated (0x004EB750 small / 0x004EBB90 large) ----
inline uint32_t GameHash8(const uint8_t* p) { return static_cast<uint32_t>(p[0] ^ p[2] ^ p[1]); }                         // 0x004EB80C
inline uint32_t GameHash16(const uint8_t* p) { return ((static_cast<uint32_t>(p[0]) << 8) | p[2]) ^ (static_cast<uint32_t>(p[1]) << 4); } // 0x004EBC4C

uint32_t GameCore(uint8_t* out, const uint8_t* src, int32_t size, uint32_t window, int32_t* table, int32_t* chain, bool fastInsert, bool smallInput) {
    std::memset(table, 0xFF, smallInput ? 0x400 : 0x40000); // memset(table, -1, ...)
    const bool write = out != nullptr;
    uint32_t o = 0;             // esi - out
    int32_t lit = 0;            // ebx / [esp+14h]
    uint32_t litStart = 0;      // [esp+18h]
    uint32_t pos = 0;           // edi
    int32_t remaining = size - 4; // [esp+48h]
    auto hashAt = [&](uint32_t p) { return smallInput ? GameHash8(src + p) : GameHash16(src + p); };
    while (remaining >= 0) {
        const int32_t maxLen = remaining < 0x404 ? remaining : 0x404; // [esp+28h]
        int32_t bestLen = 2, bestCost = 2;                            // [esp+10h], [esp+20h]
        uint32_t bestOffM1 = 0;                                       // [esp+2Ch]
        const uint32_t h = hashAt(pos);                               // [esp+30h]
        const int32_t headV = table[h];                               // [esp+38h]
        const int32_t posI = static_cast<int32_t>(pos);               // [esp+24h]
        int32_t minPos = posI - static_cast<int32_t>(window);         // [esp+34h]
        if (minPos <= 0) minPos = 0;
        int32_t cand = headV;
        if (cand >= minPos) {
            for (;;) { // 0x004EB854
                const uint8_t* c = src + cand;
                if (src[pos + bestLen] == c[bestLen]) {
                    int32_t len = 0;
                    if (static_cast<uint32_t>(maxLen) > 0) {
                        do {
                            if (src[pos + len] != c[len]) break;
                            len++;
                        } while (static_cast<uint32_t>(len) < static_cast<uint32_t>(maxLen));
                    }
                    if (static_cast<uint32_t>(len) > static_cast<uint32_t>(bestLen)) {
                        const uint32_t offM1 = static_cast<uint32_t>(posI - cand) - 1;
                        int32_t cost;
                        if (offM1 < 0x400 && static_cast<uint32_t>(len) <= 10) cost = 2;
                        else if (offM1 < 0x4000 && static_cast<uint32_t>(len) <= 0x43) cost = 3;
                        else cost = 4;
                        if (static_cast<uint32_t>(len - cost + 4) > static_cast<uint32_t>(bestLen - bestCost + 4)) {
                            bestLen = len;
                            bestCost = cost;
                            bestOffM1 = offM1;
                            if (static_cast<uint32_t>(len) >= 0x404) break;
                        }
                    }
                }
                cand = chain[static_cast<uint32_t>(cand) & window];
                if (cand < minPos) break;
            }
        }
        if (static_cast<uint32_t>(bestCost) < static_cast<uint32_t>(bestLen) && remaining >= 4) {
            while (lit > 3) { // 0x004EB940
                int32_t k = lit & ~3;
                if (k > 0x70) k = 0x70;
                lit -= k;
                if (write) out[o] = static_cast<uint8_t>((k >> 2) - 0x21);
                o++;
                if (write) std::memcpy(out + o, src + litStart, static_cast<size_t>(k));
                litStart += static_cast<uint32_t>(k);
                o += static_cast<uint32_t>(k);
            }
            const uint32_t len = static_cast<uint32_t>(bestLen), l = static_cast<uint32_t>(lit);
            if (bestCost == 2) {
                if (write) {
                    out[o] = static_cast<uint8_t>(((bestOffM1 >> 3) & 0xE0) + ((len - 3) << 2) + l);
                    out[o + 1] = static_cast<uint8_t>(bestOffM1);
                }
                o += 2;
            } else if (bestCost == 3) {
                if (write) {
                    out[o] = static_cast<uint8_t>(len + 0x7C);
                    out[o + 1] = static_cast<uint8_t>((bestOffM1 >> 8) + (l << 6));
                    out[o + 2] = static_cast<uint8_t>(bestOffM1);
                }
                o += 3;
            } else {
                if (write) {
                    out[o] = static_cast<uint8_t>((((len - 5) >> 6) & 0xFC) + ((bestOffM1 >> 12) & 0xF0) + l - 0x40);
                    out[o + 1] = static_cast<uint8_t>(bestOffM1 >> 8);
                    out[o + 2] = static_cast<uint8_t>(bestOffM1);
                    out[o + 3] = static_cast<uint8_t>(len - 5);
                }
                o += 4;
            }
            if (lit) {
                if (write) std::memcpy(out + o, src + litStart, static_cast<size_t>(lit));
                o += static_cast<uint32_t>(lit);
                lit = 0;
            }
            if (fastInsert) {
                chain[pos & window] = table[h];
                table[h] = static_cast<int32_t>(pos);
            } else {
                for (uint32_t k = 0; k < len; k++) {
                    const uint32_t h2 = hashAt(pos + k);
                    chain[(pos + k) & window] = table[h2];
                    table[h2] = static_cast<int32_t>(pos + k);
                }
            }
            pos += len;
            remaining -= bestLen;
            litStart = pos;
        } else { // 0x004EBAFA: literal
            chain[pos & window] = headV;
            table[h] = static_cast<int32_t>(pos);
            lit++;
            pos++;
            remaining--;
        }
    }
    // 0x004EB78C: the rest (pending literals and the last bytes), then the stop opcode
    int32_t rest = lit + remaining + 4;
    uint32_t from = litStart;
    while (rest > 3) {
        int32_t k = rest & ~3;
        if (k > 0x70) k = 0x70;
        rest -= k;
        if (write) out[o] = static_cast<uint8_t>((k >> 2) - 0x21);
        o++;
        if (write) std::memcpy(out + o, src + from, static_cast<size_t>(k));
        from += static_cast<uint32_t>(k);
        o += static_cast<uint32_t>(k);
    }
    if (write) out[o] = static_cast<uint8_t>(0xFC + rest);
    o++;
    if (rest) {
        if (write) std::memcpy(out + o, src + from, static_cast<size_t>(rest));
        o += static_cast<uint32_t>(rest);
    }
    return o;
}

} // namespace

Params ParamsFor(uint32_t size, uint32_t flags) {
    const uint32_t mode = (flags & 2) ? 1u : ((flags & 0x10000) ? 2u : 0u); // 0x004EC200
    Params p{0x10FB, 3, 0x1FFFF};
    if (static_cast<int32_t>(size) >= 0x1000000) { // cmp eax,1000000h; jl
        p.header = 0x90FB;
        p.sizeBytes = 4;
    }
    if (!(mode & 1)) {
        p.header |= 0x4000;
        p.window = 0x3FFF;
    }
    return p;
}

size_t ContextBytes() { return (static_cast<size_t>(kHeads) + (static_cast<size_t>(kPrevMask) + 1)) * sizeof(uint32_t); }

void InitContext(Context& ctx, void* memory) {
    ctx.head = static_cast<uint32_t*>(memory);
    ctx.prev = ctx.head + kHeads;
    std::memset(ctx.head, 0, kHeads * sizeof(uint32_t));
    ctx.base = 1; // 0 = empty
}

uint32_t Compress(Context& ctx, const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t capacity, uint32_t flags, const Effort& effort) {
    const Params p = ParamsFor(size, flags);
    Writer w{dst, capacity};
    if (!w.Room(2 + p.sizeBytes)) return kFailed;
    w.Byte(p.header >> 8);
    w.Byte(p.header & 0xFF);
    for (int i = static_cast<int>(p.sizeBytes) - 1; i >= 0; i--) w.Byte((size >> (8 * i)) & 0xFF);

    // Positions of this call are base + i; entries of earlier calls are below base and never used
    if (ctx.base == 0 || size > 0x7FFFFFF0u || ctx.base > 0xFFFFFFF0u - size) {
        std::memset(ctx.head, 0, kHeads * sizeof(uint32_t));
        ctx.base = 1;
    }
    const uint32_t base = ctx.base;
    ctx.base += size + 1;
    uint32_t* const head = ctx.head;
    uint32_t* const prev = ctx.prev;

    const uint32_t limit = size > 4 ? size - 4 : 0; // like the game, matches never reach the last 4 bytes
    const uint32_t window = p.window;
    const int maxChain = effort.maxChain > 0 ? effort.maxChain : 1;
    const uint32_t niceLen = effort.niceLen < 4 ? 4 : effort.niceLen;

    auto insert = [&](uint32_t i) {
        const uint32_t h = Hash4(Load32(src + i));
        prev[(base + i) & kPrevMask] = head[h];
        head[h] = base + i;
    };
    // Best (length - opcode bytes) at i among up to maxChain earlier positions with the same hash; 0 = none worth it
    auto find = [&](uint32_t i, uint32_t& bestLen, uint32_t& bestOff) -> int {
        const uint8_t* cur = src + i;
        const uint32_t v = Load32(cur);
        const uint32_t vp = base + i;
        const uint32_t minVp = i > window ? vp - window : base;
        uint32_t maxLen = limit - i;
        if (maxLen > kMaxLen) maxLen = kMaxLen;
        uint32_t cand = head[Hash4(v)];
        int bestGain = 0;
        bestLen = 0;
        bestOff = 0;
        for (int d = 0; d < maxChain; d++) {
            if (cand < minVp || cand >= vp) break;
            const uint32_t ci = cand - base;
            const uint8_t* c = src + ci;
            if (Load32(c) == v && (bestLen == 0 || c[bestLen] == cur[bestLen])) {
                const uint32_t len = MatchLen(c, cur, maxLen);
                const uint32_t off = i - ci;
                const int gain = static_cast<int>(len) - Cost(len, off);
                if (gain > bestGain) {
                    bestGain = gain;
                    bestLen = len;
                    bestOff = off;
                    if (len >= niceLen || len >= maxLen) break;
                }
            }
            const uint32_t next = prev[cand & kPrevMask];
            if (next >= cand) break; // end of the chain, or a link overwritten by a newer position
            cand = next;
        }
        return bestGain;
    };

    uint32_t i = 0, litFrom = 0;
    uint32_t curLen = 0, curOff = 0;
    int curGain = 0;
    bool haveCur = false;
    while (i + 4 <= limit) {
        if (!haveCur) curGain = find(i, curLen, curOff);
        haveCur = false;
        insert(i);
        if (curGain <= 0) {
            i++;
            continue;
        }
        if (effort.lazy && curLen < niceLen && i + 5 <= limit) {
            uint32_t nLen, nOff;
            const int nGain = find(i + 1, nLen, nOff);
            if (nGain > curGain) { // byte i stays a literal; take the match one byte later
                i++;
                curLen = nLen;
                curOff = nOff;
                curGain = nGain;
                haveCur = true;
                continue;
            }
        }
        uint32_t lit = i - litFrom;
        if (!FlushLiterals(w, src, litFrom, lit)) return kFailed;
        if (!EmitMatch(w, src + litFrom, lit, curLen, curOff, Cost(curLen, curOff))) return kFailed;
        const uint32_t end = i + curLen;
        // positions inside the match: all of them for short matches, a sample of long ones (their bytes repeat anyway)
        for (uint32_t k = i + 1; k < end && k + 4 <= limit; k++)
            if (curLen <= 64 || k < i + 16 || k + 16 >= end || (k & 3) == 0) insert(k);
        i = end;
        litFrom = end;
    }
    uint32_t rest = size - litFrom;
    if (!FlushLiterals(w, src, litFrom, rest)) return kFailed;
    if (!w.Room(1 + rest)) return kFailed;
    w.Byte(0xFC + rest);
    w.Copy(src + litFrom, rest);
    return w.n;
}

// The parse of Compress on one piece [from, to): the same search, lazy step and positions stored, with the hash chains
// first filled with the window before the piece, and matches that never end past the piece (nor reach the last 4 bytes
// of the input, like Compress). Tokens instead of opcodes.
uint32_t ParseSegment(Context& ctx, const uint8_t* src, uint32_t size, uint32_t flags, uint32_t index, const Effort& effort, Token* out) {
    if (index >= SegmentCount(size)) return 0;
    const uint32_t from = index * kSegmentBytes;
    const uint32_t to = size - from > kSegmentBytes ? from + kSegmentBytes : size;
    if (from + 4 > (size > 4 ? size - 4 : 0)) return 0; // a tail within the last 4 bytes: only literals (skip the window fill)
    const Params p = ParamsFor(size, flags);
    if (ctx.base == 0 || size > 0x7FFFFFF0u || ctx.base > 0xFFFFFFF0u - size) {
        std::memset(ctx.head, 0, kHeads * sizeof(uint32_t));
        ctx.base = 1;
    }
    const uint32_t base = ctx.base;
    ctx.base += size + 1;
    uint32_t* const head = ctx.head;
    uint32_t* const prev = ctx.prev;

    const uint32_t limit = size > 4 ? size - 4 : 0;
    const uint32_t end = to < limit ? to : limit; // no match reaches past this
    const uint32_t window = p.window;
    const int maxChain = effort.maxChain > 0 ? effort.maxChain : 1;
    const uint32_t niceLen = effort.niceLen < 4 ? 4 : effort.niceLen;

    auto insert = [&](uint32_t i) {
        const uint32_t h = Hash4(Load32(src + i));
        prev[(base + i) & kPrevMask] = head[h];
        head[h] = base + i;
    };
    auto find = [&](uint32_t i, uint32_t& bestLen, uint32_t& bestOff) -> int {
        bestLen = 0;
        bestOff = 0;
        uint32_t maxLen = end - i;
        if (maxLen > kMaxLen) maxLen = kMaxLen;
        if (maxLen < 4) return 0; // MatchLen always returns at least 4
        const uint8_t* cur = src + i;
        const uint32_t v = Load32(cur);
        const uint32_t vp = base + i;
        const uint32_t minVp = i > window ? vp - window : base;
        uint32_t cand = head[Hash4(v)];
        int bestGain = 0;
        for (int d = 0; d < maxChain; d++) {
            if (cand < minVp || cand >= vp) break;
            const uint32_t ci = cand - base;
            const uint8_t* c = src + ci;
            if (Load32(c) == v && (bestLen == 0 || c[bestLen] == cur[bestLen])) {
                const uint32_t len = MatchLen(c, cur, maxLen);
                const uint32_t off = i - ci;
                const int gain = static_cast<int>(len) - Cost(len, off);
                if (gain > bestGain) {
                    bestGain = gain;
                    bestLen = len;
                    bestOff = off;
                    if (len >= niceLen || len >= maxLen) break;
                }
            }
            const uint32_t next = prev[cand & kPrevMask];
            if (next >= cand) break;
            cand = next;
        }
        return bestGain;
    };

    for (uint32_t k = from > window ? from - window : 0; k < from && k + 4 <= limit; k++) insert(k); // the window before the piece
    uint32_t i = from, n = 0;
    uint32_t curLen = 0, curOff = 0;
    int curGain = 0;
    bool haveCur = false;
    while (i < end && i + 4 <= limit) {
        if (!haveCur) curGain = find(i, curLen, curOff);
        haveCur = false;
        insert(i);
        if (curGain <= 0) {
            i++;
            continue;
        }
        if (effort.lazy && curLen < niceLen && i + 1 < end && i + 5 <= limit) {
            uint32_t nLen, nOff;
            const int nGain = find(i + 1, nLen, nOff);
            if (nGain > curGain) {
                i++;
                curLen = nLen;
                curOff = nOff;
                curGain = nGain;
                haveCur = true;
                continue;
            }
        }
        out[n++] = Token{i, (curLen << 17) | (curOff - 1)};
        const uint32_t e = i + curLen;
        for (uint32_t k = i + 1; k < e && k + 4 <= limit; k++)
            if (curLen <= 64 || k < i + 16 || k + 16 >= e || (k & 3) == 0) insert(k);
        i = e;
    }
    return n;
}

void SegmentEncoder::Begin(const uint8_t* s, uint32_t sz, uint8_t* d, uint32_t capacity, uint32_t flags) {
    src = s;
    size = sz;
    dst = d;
    cap = capacity;
    n = 0;
    litFrom = 0;
    full = false;
    const Params p = ParamsFor(size, flags);
    Writer w{dst, cap};
    if (!w.Room(2 + p.sizeBytes)) {
        full = true;
        return;
    }
    w.Byte(p.header >> 8);
    w.Byte(p.header & 0xFF);
    for (int i = static_cast<int>(p.sizeBytes) - 1; i >= 0; i--) w.Byte((size >> (8 * i)) & 0xFF);
    n = w.n;
}

void SegmentEncoder::Add(const Token* tokens, uint32_t count) {
    if (full) return;
    Writer w{dst, cap};
    w.n = n;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t pos = tokens[k].pos, len = tokens[k].lenOff >> 17, off = (tokens[k].lenOff & 0x1FFFFu) + 1;
        uint32_t lit = pos - litFrom;
        if (!FlushLiterals(w, src, litFrom, lit) || !EmitMatch(w, src + litFrom, lit, len, off, Cost(len, off))) {
            full = true;
            return;
        }
        litFrom = pos + len;
    }
    n = w.n;
}

uint32_t SegmentEncoder::End() {
    if (full) return kFailed;
    Writer w{dst, cap};
    w.n = n;
    uint32_t rest = size - litFrom;
    if (!FlushLiterals(w, src, litFrom, rest) || !w.Room(1 + rest)) {
        full = true;
        return kFailed;
    }
    w.Byte(0xFC + rest);
    w.Copy(src + litFrom, rest);
    n = w.n;
    return n;
}

uint32_t CompressSegmented(Context& ctx, const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t capacity, uint32_t flags, const Effort& effort, Token* scratch) {
    SegmentEncoder enc;
    enc.Begin(src, size, dst, capacity, flags);
    const uint32_t segments = SegmentCount(size);
    for (uint32_t s = 0; s < segments && !enc.full; s++) enc.Add(scratch, ParseSegment(ctx, src, size, flags, s, effort, scratch));
    return enc.End();
}

uint32_t Decompress(uint8_t* dst, uint32_t capacity, const uint8_t* src, uint32_t srcSize) {
    if (!src) return 0;
    uint32_t left = srcSize;   // edi
    if (left < 2) return 0;
    uint32_t room = capacity;  // [esp+14h]
    const uint8_t* s = src;
    uint8_t* d = dst;
    const uint32_t hdr = (static_cast<uint32_t>(s[0]) << 8) | s[1];
    s += 2;
    left -= 2;
    uint32_t declared = 0;
    if (hdr & 0x8000) {
        if (hdr & 0x100) {
            if (left < 4) return 0;
            left -= 4;
            s += 4;
        }
        if (left < 4) return 0;
        declared = (static_cast<uint32_t>(s[0]) << 24) | (static_cast<uint32_t>(s[1]) << 16) | (static_cast<uint32_t>(s[2]) << 8) | s[3];
        s += 4;
        left -= 4;
    } else {
        if (hdr & 0x100) {
            if (left < 3) return 0;
            left -= 3;
            s += 3;
        }
        if (left < 3) return 0;
        declared = (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2];
        s += 3;
        left -= 3;
    }
    if (left < 1) return 0;
    for (;;) {
        const uint32_t a = *s++;
        left--;
        uint32_t lit = 0, len = 0;
        uintptr_t from = 0; // as an integer: the game compares it with the start of dst before reading
        if (a < 0x80) {
            if (left < 1) return 0;
            const uint32_t b = *s++;
            left--;
            lit = a & 3;
            if (lit > room) return 0;
            room -= lit;
            if (left < lit) return 0;
            left -= lit;
            for (uint32_t k = 0; k < lit; k++) *d++ = *s++;
            from = reinterpret_cast<uintptr_t>(d) - ((a & 0x60) << 3) - b - 1;
            len = ((a >> 2) & 7) + 3;
        } else if (a < 0xC0) {
            if (left < 2) return 0;
            const uint32_t b = s[0], c = s[1];
            s += 2;
            left -= 2;
            lit = b >> 6;
            if (lit > room) return 0;
            room -= lit;
            if (left < lit) return 0;
            left -= lit;
            for (uint32_t k = 0; k < lit; k++) *d++ = *s++;
            from = reinterpret_cast<uintptr_t>(d) - ((b & 0x3F) << 8) - c - 1;
            len = (a & 0x3F) + 4;
        } else if (a < 0xE0) {
            if (left < 3) return 0;
            const uint32_t b = s[0], c = s[1], e = s[2];
            s += 3;
            left -= 3;
            lit = a & 3;
            if (lit > room) return 0;
            room -= lit;
            if (left < lit) return 0;
            left -= lit;
            for (uint32_t k = 0; k < lit; k++) *d++ = *s++;
            from = reinterpret_cast<uintptr_t>(d) - ((a & 0x10) << 12) - (b << 8) - c - 1;
            len = ((a & 0x0C) << 6) + e + 5;
        } else {
            lit = ((a & 0x1F) << 2) + 4;
            if (lit > 0x70) { // stop
                lit = a & 3;
                if (lit > room) return 0;
                if (left < lit) return 0;
                for (uint32_t k = 0; k < lit; k++) *d++ = *s++;
                return declared;
            }
            if (lit > room) return 0;
            room -= lit;
            if (left < lit) return 0;
            left -= lit;
            for (uint32_t k = 0; k < lit; k++) *d++ = *s++;
            if (left < 1) return 0;
            continue;
        }
        if (from < reinterpret_cast<uintptr_t>(dst) || from >= reinterpret_cast<uintptr_t>(d)) return 0; // cmp edx,[dst]; jb / cmp edx,esi; jae
        if (len > room) return 0;
        room -= len;
        const uint8_t* f = reinterpret_cast<const uint8_t*>(from);
        for (uint32_t k = 0; k < len; k++) *d++ = *f++; // byte by byte: overlapping copies repeat
        if (left < 1) return 0;
    }
}

uint32_t GameCompress(const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t flags) {
    const Params p = ParamsFor(size, flags);
    const uint32_t mode = (flags & 2) ? 1u : ((flags & 0x10000) ? 2u : 0u);
    const bool smallInput = static_cast<int32_t>(size) <= 0x4000;
    std::vector<int32_t> table(smallInput ? 256u : 65536u), chain(static_cast<size_t>(p.window) + 1);
    uint8_t* out = nullptr;
    if (dst) {
        dst[0] = static_cast<uint8_t>(p.header >> 8);
        dst[1] = static_cast<uint8_t>(p.header);
        for (uint32_t i = 0; i < p.sizeBytes; i++) dst[2 + i] = static_cast<uint8_t>(size >> (8 * (p.sizeBytes - 1 - i)));
        out = dst + 2 + p.sizeBytes;
    }
    const uint32_t payload = GameCore(out, src, static_cast<int32_t>(size), p.window, table.data(), chain.data(), ((mode >> 1) & 1) != 0, smallInput);
    return payload + p.sizeBytes + 2;
}

} // namespace RefPackCodec

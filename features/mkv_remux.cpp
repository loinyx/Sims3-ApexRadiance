// MP4 -> MKV remux (see mkv_remux.h). Part of Apex Radiance.
//
// Reading: box tree of the MP4. Plain files give the samples from each track's stbl (stsz, stco/co64, stsc, stts,
// ctts, stss); fragmented files add them from every moof (tfhd, tfdt, trun) with the trex defaults. Edit lists shift
// the presentation times so video and sound start together.
// Writing: EBML header, one Segment with SeekHead, Info, Tracks, Clusters (SimpleBlocks in decode order, a new cluster
// at a video keyframe after 1 s or always after 5 s) and Cues (one per video keyframe cluster). Times in milliseconds.
#include "mkv_remux.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace MkvRemux {
namespace {

struct Sample {
    uint64_t off = 0;
    uint32_t size = 0;
    int64_t dts = 0;  // track timescale
    int32_t cto = 0;  // composition offset
    bool key = true;
};

struct Track {
    uint32_t id = 0;
    bool video = false, audio = false;
    uint32_t timescale = 1000;
    std::string codec;            // Matroska CodecID
    std::vector<uint8_t> priv;    // CodecPrivate
    uint32_t width = 0, height = 0, channels = 2;
    double rate = 48000.0;
    uint32_t defDur = 0, defSize = 0, defFlags = 0; // trex
    int64_t shiftMs = 0;          // edit list: added to every presentation time
    int64_t nextDts = 0;          // fragments without tfdt
    std::vector<Sample> samples;
};

// ---- reading
struct In {
    FILE* f = nullptr;
    uint64_t size = 0;
    bool Seek(uint64_t at) { return _fseeki64(f, static_cast<long long>(at), SEEK_SET) == 0; }
    bool Read(void* p, size_t n) { return fread(p, 1, n, f) == n; }
};

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }
uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

struct Box {
    uint32_t type = 0;
    uint64_t start = 0, body = 0, end = 0; // body = first byte after the header
};
constexpr uint32_t Fourcc(const char (&s)[5]) { return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint8_t(s[3]); }

bool NextBox(In& in, uint64_t at, uint64_t limit, Box& b) {
    if (at + 8 > limit) return false;
    uint8_t h[16];
    if (!in.Seek(at) || !in.Read(h, 8)) return false;
    uint64_t size = Be32(h);
    b.type = Be32(h + 4);
    b.start = at;
    b.body = at + 8;
    if (size == 1) {
        if (!in.Read(h + 8, 8)) return false;
        size = Be64(h + 8);
        b.body = at + 16;
    } else if (size == 0) {
        size = limit - at;
    }
    if (size < b.body - at || at + size > limit) return false;
    b.end = at + size;
    return true;
}

// A small box read whole into memory (moov parts and moof parts are small)
bool Load(In& in, const Box& b, std::vector<uint8_t>& out) {
    const uint64_t n = b.end - b.body;
    if (n > (256ull << 20)) return false;
    out.resize(static_cast<size_t>(n));
    return n == 0 || (in.Seek(b.body) && in.Read(out.data(), out.size()));
}

// In-memory box walking for the parts of moov / moof that are loaded
struct MemBox {
    uint32_t type = 0;
    const uint8_t* body = nullptr;
    size_t len = 0;
};
std::vector<MemBox> Children(const uint8_t* p, size_t len) {
    std::vector<MemBox> out;
    size_t at = 0;
    while (at + 8 <= len) {
        uint64_t size = Be32(p + at);
        size_t head = 8;
        if (size == 1) { if (at + 16 > len) break; size = Be64(p + at + 8); head = 16; }
        else if (size == 0) size = len - at;
        if (size < head || at + size > len) break;
        out.push_back({Be32(p + at + 4), p + at + head, static_cast<size_t>(size - head)});
        at += static_cast<size_t>(size);
    }
    return out;
}
const MemBox* Find(const std::vector<MemBox>& v, uint32_t type) {
    for (const auto& b : v) if (b.type == type) return &b;
    return nullptr;
}

// esds: ES_Descriptor -> DecoderConfigDescriptor -> DecoderSpecificInfo (the AAC AudioSpecificConfig)
bool AacConfig(const uint8_t* p, size_t len, std::vector<uint8_t>& asc) {
    size_t at = 4; // full box
    auto descr = [&](uint8_t& tag, size_t& size) {
        if (at >= len) return false;
        tag = p[at++];
        size = 0;
        for (int i = 0; i < 4 && at < len; i++) {
            const uint8_t b = p[at++];
            size = (size << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return true;
    };
    uint8_t tag = 0;
    size_t size = 0;
    if (!descr(tag, size) || tag != 0x03 || at + 3 > len) return false;
    const uint8_t flags = p[at + 2];
    at += 3;
    if (flags & 0x80) at += 2;
    if (flags & 0x40) { if (at >= len) return false; at += 1 + p[at]; }
    if (flags & 0x20) at += 2;
    if (!descr(tag, size) || tag != 0x04) return false;
    at += 13;
    if (!descr(tag, size) || tag != 0x05 || at + size > len) return false;
    asc.assign(p + at, p + at + size);
    return !asc.empty();
}

bool ReadStsd(const MemBox& stsd, Track& t) {
    if (stsd.len < 8) return false;
    const auto entries = Children(stsd.body + 8, stsd.len - 8);
    if (entries.empty()) return false;
    const MemBox& e = entries[0];
    const uint32_t fmt = e.type;
    if (fmt == Fourcc("avc1") || fmt == Fourcc("avc3") || fmt == Fourcc("hvc1") || fmt == Fourcc("hev1")) {
        if (e.len < 78) return false;
        t.width = Be16(e.body + 24);
        t.height = Be16(e.body + 26);
        const bool avc = fmt == Fourcc("avc1") || fmt == Fourcc("avc3");
        const auto kids = Children(e.body + 78, e.len - 78);
        const MemBox* cfg = Find(kids, avc ? Fourcc("avcC") : Fourcc("hvcC"));
        if (!cfg) return false;
        t.codec = avc ? "V_MPEG4/ISO/AVC" : "V_MPEGH/ISO/HEVC";
        t.priv.assign(cfg->body, cfg->body + cfg->len);
        t.video = true;
        return true;
    }
    if (fmt == Fourcc("mp4a")) {
        if (e.len < 28) return false;
        t.channels = Be16(e.body + 16);
        t.rate = Be32(e.body + 24) / 65536.0;
        const auto kids = Children(e.body + 28, e.len - 28);
        const MemBox* esds = Find(kids, Fourcc("esds"));
        if (!esds || !AacConfig(esds->body, esds->len, t.priv)) return false;
        t.codec = "A_AAC";
        t.audio = true;
        return true;
    }
    return false;
}

// Plain MP4 sample tables
void ReadStbl(const std::vector<MemBox>& stbl, Track& t) {
    const MemBox *stsz = Find(stbl, Fourcc("stsz")), *stco = Find(stbl, Fourcc("stco")), *co64 = Find(stbl, Fourcc("co64")),
                 *stsc = Find(stbl, Fourcc("stsc")), *stts = Find(stbl, Fourcc("stts")), *ctts = Find(stbl, Fourcc("ctts")),
                 *stss = Find(stbl, Fourcc("stss"));
    if (!stsz || stsz->len < 12 || (!stco && !co64) || !stsc || !stts) return;
    const uint32_t fixed = Be32(stsz->body + 4), count = Be32(stsz->body + 8);
    if (!count || (!fixed && stsz->len < 12 + 4ull * count)) return;
    std::vector<uint64_t> chunks;
    if (co64) { const uint32_t n = Be32(co64->body + 4); for (uint32_t i = 0; i < n && 8 + 8ull * (i + 1) <= co64->len; i++) chunks.push_back(Be64(co64->body + 8 + 8ull * i)); }
    else { const uint32_t n = Be32(stco->body + 4); for (uint32_t i = 0; i < n && 8 + 4ull * (i + 1) <= stco->len; i++) chunks.push_back(Be32(stco->body + 8 + 4ull * i)); }
    std::vector<Sample> out(count);
    for (uint32_t i = 0; i < count; i++) out[i].size = fixed ? fixed : Be32(stsz->body + 12 + 4ull * i);
    // stsc: runs of chunks with the same samples per chunk
    const uint32_t runs = Be32(stsc->body + 4);
    uint32_t si = 0;
    for (uint32_t r = 0; r < runs && si < count; r++) {
        if (8 + 12ull * (r + 1) > stsc->len) break;
        const uint32_t first = Be32(stsc->body + 8 + 12ull * r), per = Be32(stsc->body + 12 + 12ull * r);
        const uint32_t last = r + 1 < runs && 8 + 12ull * (r + 2) <= stsc->len ? Be32(stsc->body + 8 + 12ull * (r + 1)) - 1 : static_cast<uint32_t>(chunks.size());
        for (uint32_t c = first; c <= last && c >= 1 && c <= chunks.size() && si < count; c++) {
            uint64_t at = chunks[c - 1];
            for (uint32_t k = 0; k < per && si < count; k++) { out[si].off = at; at += out[si].size; si++; }
        }
    }
    // stts: decode deltas
    {
        const uint32_t n = Be32(stts->body + 4);
        int64_t d = 0;
        uint32_t i = 0;
        for (uint32_t e = 0; e < n && i < count && 8 + 8ull * (e + 1) <= stts->len; e++) {
            const uint32_t c = Be32(stts->body + 8 + 8ull * e), delta = Be32(stts->body + 12 + 8ull * e);
            for (uint32_t k = 0; k < c && i < count; k++) { out[i++].dts = d; d += delta; }
        }
        t.nextDts = d;
    }
    if (ctts) {
        const uint32_t n = Be32(ctts->body + 4);
        uint32_t i = 0;
        for (uint32_t e = 0; e < n && i < count && 8 + 8ull * (e + 1) <= ctts->len; e++) {
            const uint32_t c = Be32(ctts->body + 8 + 8ull * e);
            const int32_t off = static_cast<int32_t>(Be32(ctts->body + 12 + 8ull * e));
            for (uint32_t k = 0; k < c && i < count; k++) out[i++].cto = off;
        }
    }
    if (stss && t.video) {
        for (auto& s : out) s.key = false;
        const uint32_t n = Be32(stss->body + 4);
        for (uint32_t e = 0; e < n && 8 + 4ull * (e + 1) <= stss->len; e++) {
            const uint32_t k = Be32(stss->body + 8 + 4ull * e);
            if (k >= 1 && k <= count) out[k - 1].key = true;
        }
    }
    out.resize(si); // samples without a chunk are dropped
    t.samples.insert(t.samples.end(), out.begin(), out.end());
}

// Edit list: empty edits delay the track (movie timescale); the first media edit skips its media_time
void ReadElst(const MemBox& elst, uint32_t movieScale, Track& t) {
    if (elst.len < 8) return;
    const uint8_t v = elst.body[0];
    const uint32_t n = Be32(elst.body + 4);
    const size_t step = v == 1 ? 20 : 12;
    int64_t delayMs = 0;
    for (uint32_t i = 0; i < n && 8 + step * (i + 1) <= elst.len; i++) {
        const uint8_t* e = elst.body + 8 + step * i;
        const uint64_t dur = v == 1 ? Be64(e) : Be32(e);
        const int64_t media = v == 1 ? static_cast<int64_t>(Be64(e + 8)) : static_cast<int32_t>(Be32(e + 4));
        if (media < 0) { delayMs += movieScale ? static_cast<int64_t>(dur * 1000 / movieScale) : 0; continue; }
        t.shiftMs = delayMs - (t.timescale ? media * 1000 / t.timescale : 0);
        return;
    }
    t.shiftMs = delayMs;
}

bool ReadMoov(In& in, const Box& moov, std::vector<Track>& tracks, std::string& why) {
    std::vector<uint8_t> buf;
    if (!Load(in, moov, buf)) { why = "the MP4 header could not be read"; return false; }
    const auto top = Children(buf.data(), buf.size());
    uint32_t movieScale = 1000;
    if (const MemBox* mvhd = Find(top, Fourcc("mvhd")); mvhd && mvhd->len >= 24) movieScale = Be32(mvhd->body + (mvhd->body[0] == 1 ? 20 : 12));
    for (const auto& b : top) {
        if (b.type != Fourcc("trak")) continue;
        Track t;
        const auto trak = Children(b.body, b.len);
        if (const MemBox* tkhd = Find(trak, Fourcc("tkhd")); tkhd && tkhd->len >= 24) t.id = Be32(tkhd->body + (tkhd->body[0] == 1 ? 20 : 12));
        const MemBox* mdia = Find(trak, Fourcc("mdia"));
        if (!mdia) continue;
        const auto md = Children(mdia->body, mdia->len);
        if (const MemBox* mdhd = Find(md, Fourcc("mdhd")); mdhd && mdhd->len >= 24) t.timescale = Be32(mdhd->body + (mdhd->body[0] == 1 ? 20 : 12));
        if (!t.timescale) t.timescale = 1000;
        const MemBox* minf = Find(md, Fourcc("minf"));
        if (!minf) continue;
        const MemBox* stbl = Find(Children(minf->body, minf->len), Fourcc("stbl"));
        if (!stbl) continue;
        const auto st = Children(stbl->body, stbl->len);
        const MemBox* stsd = Find(st, Fourcc("stsd"));
        if (!stsd || !ReadStsd(*stsd, t)) continue; // other tracks (none expected) are left out
        if (const MemBox* edts = Find(trak, Fourcc("edts"))) {
            if (const MemBox* elst = Find(Children(edts->body, edts->len), Fourcc("elst"))) ReadElst(*elst, movieScale, t);
        }
        ReadStbl(st, t);
        tracks.push_back(std::move(t));
    }
    if (const MemBox* mvex = Find(top, Fourcc("mvex"))) {
        for (const auto& x : Children(mvex->body, mvex->len)) {
            if (x.type != Fourcc("trex") || x.len < 24) continue;
            const uint32_t id = Be32(x.body + 4);
            for (auto& t : tracks)
                if (t.id == id) { t.defDur = Be32(x.body + 12); t.defSize = Be32(x.body + 16); t.defFlags = Be32(x.body + 20); }
        }
    }
    if (tracks.empty()) { why = "the MP4 has no H.264, HEVC or AAC track"; return false; }
    return true;
}

void ReadMoof(In& in, const Box& moof, std::vector<Track>& tracks) {
    std::vector<uint8_t> buf;
    if (!Load(in, moof, buf)) return;
    uint64_t nextData = moof.end; // a traf without base offsets continues after the previous one's data
    for (const auto& traf : Children(buf.data(), buf.size())) {
        if (traf.type != Fourcc("traf")) continue;
        const auto kids = Children(traf.body, traf.len);
        const MemBox* tfhd = Find(kids, Fourcc("tfhd"));
        if (!tfhd || tfhd->len < 8) continue;
        const uint32_t fl = Be32(tfhd->body) & 0xFFFFFF;
        const uint32_t id = Be32(tfhd->body + 4);
        Track* t = nullptr;
        for (auto& x : tracks) if (x.id == id) t = &x;
        if (!t) continue;
        size_t at = 8;
        uint64_t base = moof.start;
        bool haveBase = false;
        if (fl & 0x1) { if (at + 8 > tfhd->len) continue; base = Be64(tfhd->body + at); at += 8; haveBase = true; }
        if (fl & 0x2) at += 4;
        uint32_t dDur = t->defDur, dSize = t->defSize, dFlags = t->defFlags;
        if (fl & 0x8) { if (at + 4 > tfhd->len) continue; dDur = Be32(tfhd->body + at); at += 4; }
        if (fl & 0x10) { if (at + 4 > tfhd->len) continue; dSize = Be32(tfhd->body + at); at += 4; }
        if (fl & 0x20) { if (at + 4 > tfhd->len) continue; dFlags = Be32(tfhd->body + at); at += 4; }
        if (!haveBase && !(fl & 0x20000)) base = moof.start; // default-base-is-moof (Media Foundation) or the first traf
        if (const MemBox* tfdt = Find(kids, Fourcc("tfdt")); tfdt && tfdt->len >= 8)
            t->nextDts = tfdt->body[0] == 1 && tfdt->len >= 12 ? static_cast<int64_t>(Be64(tfdt->body + 4)) : Be32(tfdt->body + 4);
        uint64_t pos = 0;
        bool posSet = false;
        for (const auto& tr : kids) {
            if (tr.type != Fourcc("trun") || tr.len < 8) continue;
            const uint32_t rf = Be32(tr.body) & 0xFFFFFF;
            const uint32_t n = Be32(tr.body + 4);
            size_t p = 8;
            if (rf & 0x1) { if (p + 4 > tr.len) break; pos = base + static_cast<int32_t>(Be32(tr.body + p)); p += 4; posSet = true; }
            else if (!posSet) { pos = haveBase || (fl & 0x20000) ? base : nextData; posSet = true; }
            uint32_t firstFlags = dFlags;
            const bool hasFirst = (rf & 0x4) != 0;
            if (hasFirst) { if (p + 4 > tr.len) break; firstFlags = Be32(tr.body + p); p += 4; }
            const size_t per = ((rf & 0x100) ? 4 : 0) + ((rf & 0x200) ? 4 : 0) + ((rf & 0x400) ? 4 : 0) + ((rf & 0x800) ? 4 : 0);
            for (uint32_t i = 0; i < n && p + per <= tr.len; i++) {
                Sample s;
                uint32_t dur = dDur, size = dSize, flags = i == 0 && hasFirst ? firstFlags : dFlags;
                if (rf & 0x100) { dur = Be32(tr.body + p); p += 4; }
                if (rf & 0x200) { size = Be32(tr.body + p); p += 4; }
                if (rf & 0x400) { flags = Be32(tr.body + p); p += 4; }
                if (rf & 0x800) { s.cto = static_cast<int32_t>(Be32(tr.body + p)); p += 4; }
                s.off = pos;
                s.size = size;
                s.dts = t->nextDts;
                s.key = t->audio || !(flags & 0x10000);
                pos += size;
                t->nextDts += dur;
                if (size && s.off + size <= in.size) t->samples.push_back(s);
            }
            nextData = pos;
        }
    }
}

// ---- writing
struct Out {
    FILE* f = nullptr;
    bool ok = true;
    uint64_t Pos() { return static_cast<uint64_t>(_ftelli64(f)); }
    void Bytes(const void* p, size_t n) { if (ok && n && fwrite(p, 1, n, f) != n) ok = false; }
    void Id(uint32_t id) {
        uint8_t b[4];
        int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
        for (int i = 0; i < n; i++) b[i] = static_cast<uint8_t>(id >> (8 * (n - 1 - i)));
        Bytes(b, n);
    }
    void Size(uint64_t v) { // shortest EBML size
        int n = 1;
        while (n < 8 && v >= (1ull << (7 * n)) - 1) n++;
        uint8_t b[8];
        for (int i = 0; i < n; i++) b[i] = static_cast<uint8_t>(v >> (8 * (n - 1 - i)));
        b[0] |= static_cast<uint8_t>(0x80 >> (n - 1));
        Bytes(b, n);
    }
    void Size8(uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = static_cast<uint8_t>(v >> (8 * (7 - i))); b[0] = 0x01; Bytes(b, 8); }
    void Uint(uint32_t id, uint64_t v) {
        int n = 1;
        while (n < 8 && (v >> (8 * n))) n++;
        Id(id); Size(n);
        uint8_t b[8];
        for (int i = 0; i < n; i++) b[i] = static_cast<uint8_t>(v >> (8 * (n - 1 - i)));
        Bytes(b, n);
    }
    void Uint8(uint32_t id, uint64_t v) { Id(id); Size(8); uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = static_cast<uint8_t>(v >> (8 * (7 - i))); Bytes(b, 8); }
    void Float(uint32_t id, double v) { uint64_t u; memcpy(&u, &v, 8); Id(id); Size(8); uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = static_cast<uint8_t>(u >> (8 * (7 - i))); Bytes(b, 8); }
    void Str(uint32_t id, const std::string& s) { Id(id); Size(s.size()); Bytes(s.data(), s.size()); }
    void Bin(uint32_t id, const std::vector<uint8_t>& v) { Id(id); Size(v.size()); Bytes(v.data(), v.size()); }
    // A master element of unknown length: its 8-byte size is patched by End
    uint64_t Begin(uint32_t id) { Id(id); const uint64_t at = Pos(); Size8(0); return at; }
    void End(uint64_t sizeAt) {
        const uint64_t end = Pos();
        if (_fseeki64(f, static_cast<long long>(sizeAt), SEEK_SET) != 0) { ok = false; return; }
        Size8(end - sizeAt - 8);
        if (_fseeki64(f, static_cast<long long>(end), SEEK_SET) != 0) ok = false;
    }
    void Patch8(uint64_t at, uint64_t v) { // the value of a Uint8 element written earlier
        const uint64_t end = Pos();
        if (_fseeki64(f, static_cast<long long>(at), SEEK_SET) != 0) { ok = false; return; }
        uint8_t b[8];
        for (int i = 0; i < 8; i++) b[i] = static_cast<uint8_t>(v >> (8 * (7 - i)));
        Bytes(b, 8);
        if (_fseeki64(f, static_cast<long long>(end), SEEK_SET) != 0) ok = false;
    }
};

constexpr uint32_t kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB, kSeekId = 0x53AB, kSeekPos = 0x53AC,
                   kInfo = 0x1549A966, kTracks = 0x1654AE6B, kCluster = 0x1F43B675, kCues = 0x1C53BB6B;

int64_t PtsMs(const Track& t, const Sample& s) { return (s.dts + s.cto) * 1000 / static_cast<int64_t>(t.timescale) + t.shiftMs; }
int64_t DtsMs(const Track& t, const Sample& s) { return s.dts * 1000 / static_cast<int64_t>(t.timescale) + t.shiftMs; }

} // namespace

bool Mp4ToMkv(const std::filesystem::path& from, const std::filesystem::path& to, std::string& why) {
    In in;
    if (_wfopen_s(&in.f, from.c_str(), L"rb") != 0 || !in.f) { why = "the MP4 could not be opened"; return false; }
    std::unique_ptr<FILE, int (*)(FILE*)> inClose(in.f, fclose);
    setvbuf(in.f, nullptr, _IOFBF, 1 << 20);
    _fseeki64(in.f, 0, SEEK_END);
    in.size = static_cast<uint64_t>(_ftelli64(in.f));
    std::vector<Track> tracks;
    std::vector<Box> moofs;
    bool haveMoov = false;
    for (uint64_t at = 0; at < in.size;) {
        Box b;
        if (!NextBox(in, at, in.size, b)) break; // a cut-off tail (crash) ends the file
        if (b.type == Fourcc("moov")) { if (!ReadMoov(in, b, tracks, why)) return false; haveMoov = true; }
        else if (b.type == Fourcc("moof")) moofs.push_back(b);
        at = b.end;
    }
    if (!haveMoov) { why = "the MP4 has no header (moov)"; return false; }
    for (const Box& m : moofs) ReadMoof(in, m, tracks);
    size_t total = 0;
    for (auto& t : tracks) {
        std::erase_if(t.samples, [&in](const Sample& s) { return !s.size || s.off + s.size > in.size; });
        total += t.samples.size();
    }
    if (!total) { why = "the MP4 has no frames"; return false; }
    // video first in the track list
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.video && !b.video; });

    Out out;
    if (_wfopen_s(&out.f, to.c_str(), L"wb") != 0 || !out.f) { why = "the MKV file could not be made"; return false; }
    setvbuf(out.f, nullptr, _IOFBF, 1 << 20);
    auto fail = [&](const char* w) { fclose(out.f); out.f = nullptr; DeleteFileW(to.c_str()); why = w; return false; };

    // EBML header
    {
        const uint64_t h = out.Begin(0x1A45DFA3);
        out.Uint(0x4286, 1); out.Uint(0x42F7, 1); out.Uint(0x42F2, 4); out.Uint(0x42F3, 8);
        out.Str(0x4282, "matroska"); out.Uint(0x4287, 4); out.Uint(0x4285, 2);
        out.End(h);
    }
    const uint64_t seg = out.Begin(kSegment);
    const uint64_t segData = out.Pos();
    // SeekHead with fixed-size positions, patched at the end
    uint64_t seekPos[3]{};
    {
        const uint64_t sh = out.Begin(kSeekHead);
        const uint32_t ids[3] = {kInfo, kTracks, kCues};
        for (int i = 0; i < 3; i++) {
            const uint64_t s = out.Begin(kSeek);
            uint8_t idb[4] = {static_cast<uint8_t>(ids[i] >> 24), static_cast<uint8_t>(ids[i] >> 16), static_cast<uint8_t>(ids[i] >> 8), static_cast<uint8_t>(ids[i])};
            out.Id(kSeekId); out.Size(4); out.Bytes(idb, 4);
            out.Id(kSeekPos); out.Size(8); seekPos[i] = out.Pos(); out.Bytes("\0\0\0\0\0\0\0\0", 8);
            out.End(s);
        }
        out.End(sh);
    }
    // Info
    int64_t endMs = 0;
    for (const auto& t : tracks) for (const auto& s : t.samples) endMs = std::max(endMs, PtsMs(t, s));
    const uint64_t infoAt = out.Pos() - segData;
    {
        const uint64_t i = out.Begin(kInfo);
        out.Uint(0x2AD7B1, 1000000); // milliseconds
        out.Str(0x4D80, "Apex Radiance");
        out.Str(0x5741, "Apex Radiance");
        out.Float(0x4489, static_cast<double>(endMs + 1));
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        std::vector<uint8_t> uid(16);
        for (size_t k = 0; k < uid.size(); k++) uid[k] = static_cast<uint8_t>((qpc.QuadPart >> ((k % 8) * 8)) ^ (k * 37) ^ (endMs >> (k % 4)));
        out.Bin(0x73A4, uid); // SegmentUID
        out.End(i);
    }
    // Tracks
    const uint64_t tracksAt = out.Pos() - segData;
    {
        const uint64_t ts = out.Begin(kTracks);
        for (size_t k = 0; k < tracks.size(); k++) {
            const Track& t = tracks[k];
            const uint64_t e = out.Begin(0xAE);
            out.Uint(0xD7, k + 1);
            out.Uint(0x73C5, 0x41504558ull + k); // TrackUID
            out.Uint(0x83, t.video ? 1 : 2);
            out.Uint(0x9C, 0);                    // no lacing
            out.Str(0x86, t.codec);
            if (!t.priv.empty()) out.Bin(0x63A2, t.priv);
            if (t.video && t.samples.size() > 1) {
                const int64_t span = t.samples.back().dts - t.samples.front().dts;
                if (span > 0) out.Uint(0x23E383, static_cast<uint64_t>(span * 1000000000LL / static_cast<int64_t>(t.timescale) / static_cast<int64_t>(t.samples.size() - 1)));
            }
            if (t.video) {
                const uint64_t v = out.Begin(0xE0);
                out.Uint(0xB0, t.width);
                out.Uint(0xBA, t.height);
                out.End(v);
            } else {
                const uint64_t a = out.Begin(0xE1);
                out.Float(0xB5, t.rate);
                out.Uint(0x9F, t.channels);
                out.End(a);
            }
            out.End(e);
        }
        out.End(ts);
    }
    if (!out.ok) return fail("the MKV header could not be written");

    // Clusters: all samples merged by decode time
    struct Ref { int64_t dtsMs; uint32_t track; uint32_t index; };
    std::vector<Ref> order;
    order.reserve(total);
    for (uint32_t k = 0; k < tracks.size(); k++)
        for (uint32_t i = 0; i < tracks[k].samples.size(); i++) order.push_back({DtsMs(tracks[k], tracks[k].samples[i]), k, i});
    std::stable_sort(order.begin(), order.end(), [](const Ref& a, const Ref& b) { return a.dtsMs < b.dtsMs; });
    struct Cue { int64_t ms; uint64_t pos; };
    std::vector<Cue> cues;
    const bool hasVideo = !tracks.empty() && tracks[0].video;
    uint64_t cluster = 0;
    bool open = false;
    int64_t clusterMs = 0;
    std::vector<uint8_t> data;
    for (const Ref& r : order) {
        const Track& t = tracks[r.track];
        const Sample& s = t.samples[r.index];
        const int64_t pts = std::max<int64_t>(0, PtsMs(t, s));
        const bool videoKey = t.video && s.key;
        const bool startNew = !open || (videoKey && pts - clusterMs >= 1000) || pts - clusterMs > 5000 || pts - clusterMs < -30000 ||
                              (!hasVideo && pts - clusterMs >= 1000);
        if (startNew) {
            if (open) out.End(cluster);
            const uint64_t pos = out.Pos() - segData;
            cluster = out.Begin(kCluster);
            clusterMs = pts;
            out.Uint(0xE7, static_cast<uint64_t>(clusterMs));
            open = true;
            if (videoKey || !hasVideo) cues.push_back({clusterMs, pos});
        }
        data.resize(s.size);
        if (!in.Seek(s.off) || !in.Read(data.data(), data.size())) return fail("the MP4 could not be read to the end");
        const int64_t rel = std::clamp<int64_t>(pts - clusterMs, -32768, 32767);
        out.Id(0xA3);
        out.Size(4 + data.size());
        const uint8_t head[4] = {static_cast<uint8_t>(0x80 | (r.track + 1)), static_cast<uint8_t>((rel >> 8) & 0xFF), static_cast<uint8_t>(rel & 0xFF),
                                 static_cast<uint8_t>(s.key ? 0x80 : 0x00)};
        out.Bytes(head, 4);
        out.Bytes(data.data(), data.size());
        if (!out.ok) return fail("the MKV could not be written (is the disk full?)");
    }
    if (open) out.End(cluster);
    // Cues
    const uint64_t cuesAt = out.Pos() - segData;
    {
        const uint64_t c = out.Begin(kCues);
        for (const Cue& q : cues) {
            const uint64_t p = out.Begin(0xBB);
            out.Uint(0xB3, static_cast<uint64_t>(q.ms));
            const uint64_t tp = out.Begin(0xB7);
            out.Uint(0xF7, 1);
            out.Uint(0xF1, q.pos);
            out.End(tp);
            out.End(p);
        }
        out.End(c);
    }
    out.Patch8(seekPos[0], infoAt);
    out.Patch8(seekPos[1], tracksAt);
    out.Patch8(seekPos[2], cuesAt);
    out.End(seg);
    if (!out.ok || fflush(out.f) != 0) return fail("the MKV could not be finished (is the disk full?)");
    fclose(out.f);
    return true;
}

} // namespace MkvRemux

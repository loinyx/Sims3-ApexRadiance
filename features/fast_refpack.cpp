#include "ui/widgets.h"
#include "developer_settings.h"
// Faster cache compression: the RefPack stream write answered by a fast compressor with the game's stream format (see
// fast_refpack.h, features/refpack_codec.h and docs/features/performance.md).
//
// Part of Apex Radiance. Credits: @loinyx
#include "fast_refpack.h"
#include "memory_policy.h"
#include "memory_telemetry.h"
#include "refpack_codec.h"
#include "dxt_codec.h"
#include "slot_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include <windows.h>
#include <intrin.h>
#include <nmmintrin.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

namespace FastRefPack {
namespace {

using SlotChain::Layer;
using SlotChain::Site;
using FnThis5 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using FnDecompress = uint32_t(__cdecl*)(uint8_t* dst, uint32_t capacity, const uint8_t* src, uint32_t srcSize);

constexpr int kPool = 4;
constexpr uint32_t kStartupChecks = 16;
constexpr uint32_t kKeepScratch = 1u << 20; // check buffers up to 1 MB stay allocated per context
constexpr uint32_t kNiceLen = 96;
constexpr uint64_t kPairGraceMs = 2000;

struct Slot {
    std::atomic<bool> busy{false};
    uint64_t lastUsed = 0; // guarded by busy
    void* mem = nullptr; // RefPackCodec::ContextBytes(), guarded by busy
    RefPackCodec::Context ctx;
    uint8_t* scratch = nullptr;
    uint32_t scratchSize = 0;
    RefPackCodec::Token* tokens = nullptr; // kMaxSegmentTokens, for a large stream compressed on this thread alone
};
Slot g_pool[kPool];

// The last counting run of this thread: its write must use the same compressor, flags and search depth (the buffer was
// sized by that run; the package writer counts with flags 2 and may write with other flags)
struct Pair {
    uint32_t src = 0, size = 0, flags = 0;
    int chain = 0;
    bool fast = false, valid = false;
};
thread_local Pair t_pair;

std::mutex g_ctrl;
std::atomic<bool> g_installed{false}; // the slot-chain layer (changed under g_ctrl)
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};
std::atomic<uint64_t> g_lastFastCountTick{0};
std::atomic<int> g_verifyEvery{kPublicBuild ? 0 : 8}; // dev: 1 in 8 after the first 16 (every stream until 30/09: its decode was 19% of the compression hitches)
std::atomic<int> g_compareEvery{0};
std::atomic<int> g_chain{32};
std::atomic<uint32_t> g_seq{0};
FnDecompress g_decoder = nullptr; // the game's decompressor (0x004EB3B0), else Apex's translation
std::atomic<uint64_t> c_streams{0}, c_in{0}, c_out{0}, c_count{0}, c_passed{0}, c_paired{0}, c_overflow{0}, c_temp{0};
std::atomic<uint64_t> c_checked{0}, c_mismatch{0}, c_notCheckable{0}, c_compared{0}, c_cmpGame{0}, c_cmpFast{0};
std::atomic<uint64_t> g_ticks{0}, g_countTicks{0}, g_cmpGameTicks{0}, g_cmpFastTicks{0};
double g_qpcMs = 0.0;
std::mutex g_mismatchLock;
std::string g_lastMismatch;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

// Large-stream compression needs both context and tokens. Small streams and verification need no token array.
bool InitSlot(Slot& s, bool needTokens=true) {
    if (s.mem && (!needTokens || s.tokens)) return true;
    if (!s.mem) s.mem = VirtualAlloc(nullptr, RefPackCodec::ContextBytes(), MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE);
    if (!s.mem) return false;
    if (needTokens && !s.tokens) s.tokens = static_cast<RefPackCodec::Token*>(VirtualAlloc(nullptr, RefPackCodec::kMaxSegmentTokens * sizeof(RefPackCodec::Token), MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE));
    if (needTokens && !s.tokens) return false;
    if (!s.ctx.head) RefPackCodec::InitContext(s.ctx, s.mem);
    return true;
}

Slot* Acquire(bool needTokens=true) {
    for (Slot& s : g_pool) {
        if (s.busy.exchange(true, std::memory_order_acquire)) continue;
        if (InitSlot(s,needTokens)) return &s;
        s.busy.store(false, std::memory_order_release);
        return nullptr;
    }
    return nullptr;
}

// ---- Large streams (more than one piece of RefPackCodec::kSegmentBytes): the segmented compressor, its pieces parsed on
// the calling thread and a small pool of worker threads. The bytes do not depend on who parsed which piece, so a stream
// compressed on the calling thread alone (the pool busy with another thread's stream, or no worker) is the same.
constexpr uint32_t kMaxWorkers = 6;
constexpr uint32_t kClosed = 0x80000000u;    // ParPool::state: the round takes no more threads
constexpr uint32_t kCountMask = 0x7FFFFFFFu; // ParPool::state: threads checked in
constexpr SIZE_T kWorkerStack = 256 * 1024;  // reserved address space per worker (the game is 32-bit)
constexpr uint32_t kPiecesPerThread = 2;     // pieces per thread and round: faster threads take more (offline: 3.8x vs 3.2x with 1)
constexpr uint32_t kRoundMax = kPiecesPerThread * (kMaxWorkers + 1);

std::atomic<uint32_t> g_workersMade{0}; // for the developer UI (reading the pool would create it)

struct Round {
    const uint8_t* src = nullptr;
    uint32_t size = 0, flags = 0, first = 0, count = 0; // pieces first .. first + count - 1
    RefPackCodec::Effort effort;
    RefPackCodec::Token* tokens[kRoundMax] = {}; // one buffer per piece of the round
    uint32_t counts[kRoundMax] = {};
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> byWorkers{0};
};

void RunPieces(Round& r, RefPackCodec::Context& ctx, bool worker) {
    for (;;) {
        const uint32_t k = r.next.fetch_add(1, std::memory_order_relaxed);
        if (k >= r.count) break;
        r.counts[k] = RefPackCodec::ParseSegment(ctx, r.src, r.size, r.flags, r.first + k, r.effort, r.tokens[k]);
        if (worker) r.byWorkers.fetch_add(1, std::memory_order_relaxed);
    }
}

// Pool threads live until process exit; idle worker contexts can be retired under pressure. The hand-off is the one
// of the texture encoder's pool (features/dxt_codec.cpp): the caller owns the pool (inUse), writes the round while it is
// closed, opens it, wakes k workers, parses pieces too, closes it, then waits until no thread is checked in; a worker
// wakes, checks in, parses pieces only if the round is open, checks out; the one whose check-out makes the count 0 while
// closed sets `done` (the caller re-checks the count around every wait).
struct ParPool {
    std::atomic<bool> inUse{false};
    std::atomic<uint32_t> state{kClosed};
    std::atomic<uint32_t> created{0};
    std::mutex createLock;
    bool createFailed = false; // guarded by createLock
    HANDLE done = nullptr;
    struct Worker {
        HANDLE wake = nullptr;
        void* mem = nullptr;
        RefPackCodec::Context ctx;
        ParPool* pool = nullptr;
    } workers[kMaxWorkers];
    RefPackCodec::Token* tokens[kRoundMax] = {}; // guarded by inUse
    Round round;
    std::atomic<uint64_t> lastUsed{0};

    ParPool() { done = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

    static DWORD WINAPI WorkerMain(void* p) {
        Worker& w = *static_cast<Worker*>(p);
        ParPool& pool = *w.pool;
        for (;;) {
            if (WaitForSingleObject(w.wake, INFINITE) != WAIT_OBJECT_0) {
                Sleep(10);
                continue;
            }
            const uint32_t s = pool.state.fetch_add(1, std::memory_order_acq_rel);
            // A late wake from an older, wider round can arrive after a smaller
            // round opens. Retired helpers outside that round's prefix have no context.
            if (!(s & kClosed) && w.mem) RunPieces(pool.round, w.ctx, true);
            if (pool.state.fetch_sub(1, std::memory_order_acq_rel) == (kClosed | 1u)) SetEvent(pool.done);
        }
    }

    // At least n workers when possible; returns how many can be used (<= n)
    uint32_t EnsureWorkers(uint32_t n) {
        if (n > kMaxWorkers) n = kMaxWorkers;
        uint32_t have = created.load(std::memory_order_acquire);
        bool ready=have>=n;
        if (ready) for(uint32_t i=0;i<n;++i) ready=ready && workers[i].mem;
        if (ready) return n;
        std::lock_guard<std::mutex> lock(createLock);
        have = created.load(std::memory_order_relaxed);
        // The pool owner restores retired contexts before opening any round.
        for(uint32_t i=0;i<std::min(have,n);++i) {
            auto& w=workers[i];
            if (!w.mem) {
                w.mem=VirtualAlloc(nullptr,RefPackCodec::ContextBytes(),MEM_COMMIT|MEM_RESERVE|(kMemoryOptimizationsEnabled?MEM_TOP_DOWN:0),PAGE_READWRITE);
                if (!w.mem) return i;
                RefPackCodec::InitContext(w.ctx,w.mem);
            }
        }
        if (!done) createFailed = true;
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static const auto setDescription = reinterpret_cast<SetDescription>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
        while (have < n && !createFailed) {
            Worker& w = workers[have];
            w.pool = this;
            w.mem = VirtualAlloc(nullptr, RefPackCodec::ContextBytes(), MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE);
            w.wake = w.mem ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
            if (!w.wake) {
                if (w.mem) VirtualFree(w.mem, 0, MEM_RELEASE);
                w.mem = nullptr;
                createFailed = true;
                break;
            }
            RefPackCodec::InitContext(w.ctx, w.mem);
            // Normal priority on purpose (as the texture encoder's workers): the calling thread waits for this stream
            const HANDLE h = CreateThread(nullptr, kWorkerStack, &ParPool::WorkerMain, &w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (!h) {
                CloseHandle(w.wake);
                w.wake = nullptr;
                VirtualFree(w.mem, 0, MEM_RELEASE);
                w.mem = nullptr;
                createFailed = true;
                break;
            }
            if (setDescription) setDescription(h, L"Apex RefPack worker");
            CloseHandle(h); // never joined
            have++;
            created.store(have, std::memory_order_release);
            g_workersMade.store(have, std::memory_order_relaxed);
        }
        return have < n ? have : n;
    }

    // Token buffers for k pieces per round (caller owns the pool)
    bool EnsureTokens(uint32_t k) {
        for (uint32_t i = 0; i < k; i++)
            if (!tokens[i]) {
                tokens[i] = static_cast<RefPackCodec::Token*>(VirtualAlloc(nullptr, RefPackCodec::kMaxSegmentTokens * sizeof(RefPackCodec::Token), MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE));
                if (!tokens[i]) return false;
            }
        return true;
    }
};

std::atomic<ParPool*> g_existingPool{nullptr};
ParPool& ThePool() {
    static ParPool* const pool = [] { auto* p=new ParPool(); g_existingPool.store(p,std::memory_order_release); return p; }(); // intentionally leaked (see ParPool)
    return *pool;
}

std::atomic<uint64_t> c_large{0}, c_largeParallel{0}, c_largeBusy{0}, c_pieces{0}, c_piecesByWorkers{0};

// A large stream: the segmented compressor, on the pool when it is free. Same contract as RefPackCodec::Compress.
uint32_t CompressLarge(Slot& slot, const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t capacity, uint32_t flags, const RefPackCodec::Effort& effort) {
    const uint32_t pieces = RefPackCodec::SegmentCount(size);
    c_large.fetch_add(1, std::memory_order_relaxed);
    c_pieces.fetch_add(pieces, std::memory_order_relaxed);
    auto alone = [&]() -> uint32_t {
        if (!slot.tokens) slot.tokens = static_cast<RefPackCodec::Token*>(VirtualAlloc(nullptr, RefPackCodec::kMaxSegmentTokens * sizeof(RefPackCodec::Token), MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE));
        // out of memory: the plain compressor (a write whose count was segmented may then not fit: -1, the data is stored
        // uncompressed)
        if (!slot.tokens) return RefPackCodec::Compress(slot.ctx, src, size, dst, capacity, flags, effort);
        return RefPackCodec::CompressSegmented(slot.ctx, src, size, dst, capacity, flags, effort, slot.tokens);
    };
    const uint32_t want = DxtCodec::Parallel::DefaultWorkers();
    if (!want || pieces < 2) return alone();
    ParPool& p = ThePool();
    if (p.inUse.exchange(true, std::memory_order_acquire)) {
        c_largeBusy.fetch_add(1, std::memory_order_relaxed);
        return alone();
    }
    const uint32_t usefulWorkers = std::min(want,std::min(kMaxWorkers,pieces-1));
    uint32_t k = p.EnsureWorkers(usefulWorkers);
    const uint32_t perRound = kPiecesPerThread * (k + 1);
    const uint32_t usefulPieces=std::min(perRound,pieces);
    if (!k || !p.EnsureTokens(usefulPieces)) {
        p.lastUsed.store(GetTickCount64(),std::memory_order_relaxed);
        p.inUse.store(false, std::memory_order_release);
        return alone();
    }
    RefPackCodec::SegmentEncoder enc;
    enc.Begin(src, size, dst, capacity, flags);
    Round& r = p.round;
    uint32_t byWorkers = 0;
    for (uint32_t first = 0; first < pieces && !enc.full; first += perRound) {
        // written while the state is closed: threads that check in now (late wakes) leave without reading it
        r.src = src;
        r.size = size;
        r.flags = flags;
        r.effort = effort;
        r.first = first;
        r.count = pieces - first < perRound ? pieces - first : perRound;
        for (uint32_t i = 0; i < r.count; i++) r.tokens[i] = p.tokens[i];
        r.next.store(0, std::memory_order_relaxed);
        r.byWorkers.store(0, std::memory_order_relaxed);
        p.state.fetch_and(~kClosed, std::memory_order_release); // open
        const uint32_t wake = r.count - 1 < k ? r.count - 1 : k;
        for (uint32_t i = 0; i < wake; i++) SetEvent(p.workers[i].wake);
        RunPieces(r, slot.ctx, false); // the caller parses too; when it finds none left, every piece is taken
        p.state.fetch_or(kClosed, std::memory_order_acq_rel);
        for (int spin = 0; (p.state.load(std::memory_order_acquire) & kCountMask) != 0; spin++) {
            if (spin < 4000) _mm_pause();
            else WaitForSingleObject(p.done, 50);
        }
        byWorkers += r.byWorkers.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < r.count; i++) enc.Add(r.tokens[i], r.counts[i]);
    }
    p.lastUsed.store(GetTickCount64(),std::memory_order_relaxed);
    p.inUse.store(false, std::memory_order_release);
    c_largeParallel.fetch_add(1, std::memory_order_relaxed);
    c_piecesByWorkers.fetch_add(byWorkers, std::memory_order_relaxed);
    return enc.End();
}

// The fast compressor for any stream: large ones segmented (see above). The size alone decides, so a counting run and its
// write always use the same one.
uint32_t CompressAny(Slot& slot, const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t capacity, uint32_t flags, const RefPackCodec::Effort& effort) {
    if (size > RefPackCodec::kSegmentBytes) return CompressLarge(slot, src, size, dst, capacity, flags, effort);
    return RefPackCodec::Compress(slot.ctx, src, size, dst, capacity, flags, effort);
}

// ---- The counting run's stream, kept for its write ----
// The package writer measures a stream (no destination), allocates, then writes it: the same compression twice. The
// counting run now writes into a buffer of this thread and its write copies it, when the source still has the same CRC-32C
// (checked with the SSE4.2 instruction; without it, or when anything differs, the write compresses again as before).
constexpr uint32_t kMinHeld = kMemoryOptimizationsEnabled ? 64u<<10 : 1u<<20;
constexpr uint32_t kKeepHeld = 8u << 20; // buffers up to 8 MB stay allocated per thread (a 5.5 MB Sim cache: 6.9 MB), so the next
                                         // large stream does not allocate and fault its pages in again
constexpr uint32_t kKeepHeldTotal = 12u << 20; // ... and all threads together keep at most this much (a 32-bit game's address space:
                                              // up to 8 MB per thread was kept before 30/09)
std::atomic<uint32_t> g_heldBytes{0};         // allocated held buffers of all threads
struct Held {
    uint8_t* buf = nullptr;
    uint32_t cap = 0, len = 0, src = 0, size = 0, flags = 0, crc = 0;
    int chain = 0;
    bool valid = false;
    void Free() {
        if (!buf) return;
        if (!VirtualFree(buf, 0, MEM_RELEASE)) return;
        g_heldBytes.fetch_sub(cap, std::memory_order_relaxed);
        buf = nullptr;
        cap = 0;
    }
    void Alloc(uint32_t want) {
        if constexpr (kMemoryOptimizationsEnabled) {
            // Includes active count/write copies, not just retained idle buffers.
            const uint32_t limit=MemoryTelemetry::saveDepth.load(std::memory_order_acquire) ? 8u<<20 :
                MemoryTelemetry::pressure.load(std::memory_order_relaxed) ? 32u<<20 : 64u<<20;
            auto previous=g_heldBytes.load(std::memory_order_relaxed);
            do {
                if (want>limit || previous>limit-want) return;
            } while (!g_heldBytes.compare_exchange_weak(previous,previous+want,std::memory_order_relaxed));
            buf=static_cast<uint8_t*>(VirtualAlloc(nullptr,want,MEM_COMMIT|MEM_RESERVE|MEM_TOP_DOWN,PAGE_READWRITE));
            cap=buf ? want : 0;
            if (!buf) g_heldBytes.fetch_sub(want,std::memory_order_relaxed);
            return;
        }
        buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, want, MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE));
        cap = buf ? want : 0;
        g_heldBytes.fetch_add(cap, std::memory_order_relaxed);
    }
    ~Held() { Free(); } // the thread ends
};
thread_local Held t_held;
std::atomic<uint64_t> c_reused{0}, c_reuseMissed{0};

bool CpuHasCrc32() {
    static const bool yes = [] {
        int r[4] = {};
        __cpuid(r, 1);
        return (r[2] & (1 << 20)) != 0; // SSE4.2
    }();
    return yes;
}

// CRC-32C of the source, four interleaved lanes (one per 4-byte word of each 16 bytes), then the tail
uint32_t SourceCrc(const uint8_t* p, uint32_t n) {
    uint32_t c0 = 0xFFFFFFFFu, c1 = 0x9E3779B9u, c2 = 0x85EBCA6Bu, c3 = 0xC2B2AE35u;
    uint32_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint32_t w[4];
        std::memcpy(w, p + i, 16);
        c0 = _mm_crc32_u32(c0, w[0]);
        c1 = _mm_crc32_u32(c1, w[1]);
        c2 = _mm_crc32_u32(c2, w[2]);
        c3 = _mm_crc32_u32(c3, w[3]);
    }
    for (; i < n; i++) c0 = _mm_crc32_u8(c0, p[i]);
    return c0 ^ _rotl(c1, 8) ^ _rotl(c2, 16) ^ _rotl(c3, 24) ^ n;
}

void DropHeld() {
    Held& h = t_held;
    h.valid = false;
    const bool saving=kMemoryOptimizationsEnabled && MemoryTelemetry::saveDepth.load(std::memory_order_acquire);
    if (h.buf && (h.cap > kKeepHeld || g_heldBytes.load(std::memory_order_relaxed) > kKeepHeldTotal ||
        (saving && h.cap>64u*1024) || (kMemoryOptimizationsEnabled && MemoryTelemetry::pressure.load(std::memory_order_relaxed)))) h.Free();
}

// Decompresses the stream with the game's decoder and compares it with the source. False only on a real difference.
bool Check(Slot& slot, const uint8_t* stream, uint32_t streamSize, const uint8_t* src, uint32_t size) {
    uint8_t* buf = nullptr;
    bool own = false;
    if (size <= kKeepScratch) {
        if (!slot.scratch || slot.scratchSize < size) {
            if (slot.scratch) VirtualFree(slot.scratch, 0, MEM_RELEASE);
            const uint32_t want = size < 0x10000 ? 0x10000 : ((size + 0xFFFF) & ~0xFFFFu);
            slot.scratch = static_cast<uint8_t*>(VirtualAlloc(nullptr, want, MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE));
            slot.scratchSize = slot.scratch ? want : 0;
        }
        buf = slot.scratch;
    } else {
        buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE | (kMemoryOptimizationsEnabled ? MEM_TOP_DOWN : 0), PAGE_READWRITE));
        own = true;
    }
    if (!buf) {
        c_notCheckable.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const uint32_t got = g_decoder ? g_decoder(buf, size, stream, streamSize) : RefPackCodec::Decompress(buf, size, stream, streamSize);
    bool ok = got == size;
    uint32_t at = 0;
    if (ok && std::memcmp(buf, src, size) != 0) {
        ok = false;
        while (at < size && buf[at] == src[at]) at++;
    }
    if (own) VirtualFree(buf, 0, MEM_RELEASE);
    if (!ok) {
        const std::string msg = std::format("{} bytes -> {}-byte stream (header {:02x}{:02x}); the {} decoder returned {}{}", size, streamSize, static_cast<unsigned>(stream[0]), static_cast<unsigned>(stream[1]),
                                            g_decoder ? "game's" : "Apex copy of the", got, got == size ? std::format(", first different byte at {}", at) : std::string());
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        if (g_lastMismatch.empty()) LOG_ERROR("[FastRefPack] Verification mismatch: " + msg + ". The game's compressor was used; the feature turns itself off for this session.");
        g_lastMismatch = msg;
    }
    return ok;
}

// The stream write (layer FastCompress of the slot chain): thiscall(src, size, dst, capacity, flags), ret 14h
uint64_t __fastcall Hook_StreamWrite(void* self, void* edx, uint32_t src, uint32_t size, uint32_t dst, uint32_t capacity, uint32_t flags) {
    const FnThis5 next = reinterpret_cast<FnThis5>(SlotChain::Next(Site::RefPackCompress, Layer::FastCompress));
    if (!dst && (flags & 1)) return next(self, edx, src, size, dst, capacity, flags); // the size bound: no work
    const bool counting = dst == 0;
    Pair& pr = t_pair;
    bool fast;
    uint32_t useFlags = flags;
    int chain = g_chain.load(std::memory_order_relaxed);
    if (!counting && pr.valid && pr.src == src && pr.size == size) { // the write of a stream this thread just counted
        fast = pr.fast;
        pr.valid = false;
        if (fast) {
            useFlags = pr.flags; // same flags and depth as the counting run: the stream has exactly the counted size
            chain = pr.chain;
            c_paired.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        fast = g_on.load(std::memory_order_acquire) && !g_selfDisabled.load(std::memory_order_relaxed);
    }
    if (!fast) {
        if (counting) pr = Pair{src, size, flags, 0, false, true};
        c_passed.fetch_add(1, std::memory_order_relaxed);
        return next(self, edx, src, size, dst, capacity, flags);
    }
    Slot temp;
    Slot* slot = nullptr;
    auto ensureSlot = [&](bool compressing) {
        if (slot) return true;
        const bool tokens=compressing && size>RefPackCodec::kSegmentBytes;
        slot=Acquire(tokens);
        if (slot) return true;
        if (!InitSlot(temp,tokens)) {
            if (temp.mem) {VirtualFree(temp.mem,0,MEM_RELEASE);temp.mem=nullptr;}
            return false;
        }
        c_temp.fetch_add(1,std::memory_order_relaxed);
        slot=&temp;
        return true;
    };
    auto fallback = [&]() -> uint64_t {
        if (counting || capacity==0 || capacity>=RefPackCodec::SizeBound(size)) {
            if (counting) pr=Pair{src,size,flags,0,false,true};
            c_passed.fetch_add(1,std::memory_order_relaxed);
            return next(self,edx,src,size,dst,capacity,flags);
        }
        return RefPackCodec::kFailed;
    };
    auto finish = [&] {
        if (!slot) return;
        if (slot == &temp) {
            VirtualFree(temp.mem, 0, MEM_RELEASE);
            if (temp.scratch) VirtualFree(temp.scratch, 0, MEM_RELEASE);
            if (temp.tokens) VirtualFree(temp.tokens, 0, MEM_RELEASE);
        } else {
            slot->lastUsed=GetTickCount64();
            slot->busy.store(false, std::memory_order_release);
        }
    };
    RefPackCodec::Effort effort;
    effort.maxChain = chain;
    effort.niceLen = kNiceLen;
    effort.lazy = true;
    const uint8_t* const s = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(src));
    uint8_t* const d = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(dst));
    const uint64_t t0 = Qpc();
    uint32_t r = RefPackCodec::kFailed;
    bool reused = false;
    Held& h = t_held;
    if (counting) { // compress into this thread's buffer, kept for the write (else count only, as before)
        if (!ensureSlot(true)) return fallback();
        h.valid = false;
        const uint32_t bound = RefPackCodec::SizeBound(size);
        if (CpuHasCrc32() && bound > size) {
            // a buffer larger than needed is kept only while it serves streams that large (a 32-bit game)
            if (h.buf && (h.cap < bound || (h.cap > kKeepHeld && bound <= kKeepHeld))) h.Free();
            if (!h.buf) h.Alloc(bound < kMinHeld ? kMinHeld : bound);
            if (h.buf) {
                const uint32_t crc = SourceCrc(s, size); // before compressing: a source changed meanwhile never matches
                r = CompressAny(*slot, s, size, h.buf, h.cap, useFlags, effort);
                if (r != RefPackCodec::kFailed) {
                    h.len = r, h.src = src, h.size = size, h.flags = useFlags, h.chain = chain;
                    h.crc = crc;
                    h.valid = true;
                }
            }
        }
        if (r == RefPackCodec::kFailed) r = CompressAny(*slot, s, size, nullptr, 0, useFlags, effort);
    } else {
        if (h.valid && h.src == src && h.size == size) { // the stream this thread counted last
            if (h.flags == useFlags && h.chain == chain && SourceCrc(s, size) == h.crc) {
                r = capacity && capacity < h.len ? RefPackCodec::kFailed : h.len;
                if (r != RefPackCodec::kFailed) std::memcpy(d, h.buf, r);
                reused = true;
                c_reused.fetch_add(1, std::memory_order_relaxed);
            } else {
                c_reuseMissed.fetch_add(1, std::memory_order_relaxed);
            }
            DropHeld();
        }
        if (!reused) {
            if (!ensureSlot(true)) return fallback();
            r = CompressAny(*slot, s, size, d, capacity, useFlags, effort);
        }
    }
    const uint64_t t1 = Qpc();
    if (counting) {
        pr = Pair{src, size, flags, chain, true, true};
        g_lastFastCountTick.store(GetTickCount64(), std::memory_order_relaxed);
        c_count.fetch_add(1, std::memory_order_relaxed);
        g_countTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
        finish();
        return r;
    }
    if (r == RefPackCodec::kFailed) { // does not fit: the callers store the data uncompressed
        c_overflow.fetch_add(1, std::memory_order_relaxed);
        finish();
        return RefPackCodec::kFailed;
    }
    c_streams.fetch_add(1, std::memory_order_relaxed);
    c_in.fetch_add(size, std::memory_order_relaxed);
    c_out.fetch_add(r, std::memory_order_relaxed);
    g_ticks.fetch_add(t1 - t0, std::memory_order_relaxed);
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    const int ve = !kPublicBuild ? g_verifyEvery.load(std::memory_order_relaxed) : 0;
    if (seq < kStartupChecks || (ve > 0 && seq % static_cast<uint32_t>(ve) == 0)) {
        if (!ensureSlot(false)) {c_notCheckable.fetch_add(1,std::memory_order_relaxed);return r;}
        if (!Check(*slot, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(dst)), r, s, size)) {
            c_mismatch.fetch_add(1, std::memory_order_relaxed);
            g_selfDisabled.store(true);
            finish();
            if (capacity == 0 || capacity >= RefPackCodec::SizeBound(size)) return next(self, edx, src, size, dst, capacity, flags);
            return RefPackCodec::kFailed;
        }
        c_checked.fetch_add(1, std::memory_order_relaxed);
    }
    finish();
    const int ce = g_compareEvery.load(std::memory_order_relaxed);
    if (!kPublicBuild && !reused && ce > 0 && seq % static_cast<uint32_t>(ce) == 0) { // the game's compressor on the same data, counting only
        const uint64_t g0 = Qpc();
        const uint32_t gs = static_cast<uint32_t>(next(self, edx, src, size, 0, 0, useFlags & ~1u)); // same flags as ours; bit 0 off = compress, not the size bound
        const uint64_t g1 = Qpc();
        c_compared.fetch_add(1, std::memory_order_relaxed);
        c_cmpGame.fetch_add(gs, std::memory_order_relaxed);
        c_cmpFast.fetch_add(r, std::memory_order_relaxed);
        g_cmpGameTicks.fetch_add(g1 - g0, std::memory_order_relaxed);
        g_cmpFastTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
    }
    return r;
}


// Only Apex-owned storage is retired. Slots have no retained references between
// calls; the count/write pairing keeps flags and bytes in thread-local Held.
uint64_t RetireIdleStorage(uint64_t now=GetTickCount64(),bool preparingSave=false) {
    if constexpr (!kMemoryOptimizationsEnabled) return 0;
    static std::atomic<uint64_t> last{0};
    auto previous=last.load(std::memory_order_relaxed);
    if (!preparingSave && (now-previous<5000 || !last.compare_exchange_strong(previous,now,std::memory_order_relaxed))) return 0;
    if (preparingSave) last.store(now,std::memory_order_relaxed);
    uint64_t freed=0;
    for (Slot& s:g_pool) {
        if (s.busy.exchange(true,std::memory_order_acquire)) continue;
        if (preparingSave || MemoryPolicy::Expired(now,s.lastUsed,120000)) {
            if (s.mem && VirtualFree(s.mem,0,MEM_RELEASE)) { s.mem=nullptr; s.ctx={}; freed+=RefPackCodec::ContextBytes(); }
            if (s.tokens && VirtualFree(s.tokens,0,MEM_RELEASE)) { s.tokens=nullptr; freed+=RefPackCodec::kMaxSegmentTokens*sizeof(RefPackCodec::Token); }
            if (s.scratch && VirtualFree(s.scratch,0,MEM_RELEASE)) { s.scratch=nullptr; freed+=s.scratchSize; s.scratchSize=0; }
            s.lastUsed=now;
        }
        s.busy.store(false,std::memory_order_release);
    }
    if (auto* p=g_existingPool.load(std::memory_order_acquire)) {
        if (!p->inUse.exchange(true,std::memory_order_acquire)) {
            // Closed late wakes do not inspect contexts or token pointers.
            if (p->state.load(std::memory_order_acquire)==kClosed &&
                (preparingSave || MemoryPolicy::Expired(now,p->lastUsed.load(),120000))) {
                for (auto& token:p->tokens)
                    if (token && VirtualFree(token,0,MEM_RELEASE)) {
                        token=nullptr; freed+=RefPackCodec::kMaxSegmentTokens*sizeof(RefPackCodec::Token);
                    }
                if (preparingSave || MemoryTelemetry::pressure.load(std::memory_order_relaxed))
                    for(auto& worker:p->workers)
                        if (worker.mem && VirtualFree(worker.mem,0,MEM_RELEASE)) {
                            worker.mem=nullptr;worker.ctx={};freed+=RefPackCodec::ContextBytes();
                        }
                p->lastUsed.store(now);
            }
            p->inUse.store(false,std::memory_order_release);
        }
    }
    if (preparingSave) {
        auto& held=t_held;
        if(!held.valid && held.buf){const auto before=held.cap;held.Free();if(!held.buf)freed+=before;}
    } else if (freed) LOG_INFO(std::format("[MemoryBudget] Retired {} KiB of idle Apex RefPack storage after 120 s",freed>>10));
    return freed;
}

void RemoveLayerLocked() {
    if (!g_installed.load()) return;
    SlotChain::Remove(Site::RefPackCompress, Layer::FastCompress);
    g_installed.store(false);
}

} // namespace

uint64_t PrepareForSave() {return RetireIdleStorage(GetTickCount64(),true);}

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_on.load()) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("FastCacheCompression", &missing)) return fail(GameAddr::NotAvailable(missing));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    g_decoder = reinterpret_cast<FnDecompress>(GameAddr::Get(GameAddr::Id::RefPackDecompress));
    if (!g_installed.load()) {
        std::string err;
        if (!SlotChain::Install(Site::RefPackCompress, Layer::FastCompress, reinterpret_cast<void*>(&Hook_StreamWrite), &err)) return fail("Could not hook the RefPack stream write: " + err);
        g_installed.store(true);
    }
    g_selfDisabled.store(false);
    g_seq.store(0); // the startup checks run again after every start
    g_on.store(true, std::memory_order_release);
    LOG_INFO(std::format("[FastRefPack] On: the RefPack stream write {:#010x} answered through its vtable slot; streams checked with {} (the first {} always)",
                         SlotChain::GameFunction(Site::RefPackCompress), g_decoder ? std::format("the game's decoder {:#010x}", reinterpret_cast<uintptr_t>(g_decoder)) : std::string("Apex's copy of the decoder"),
                         kStartupChecks));
    return true;
}

void Stop() {
    {
        std::lock_guard<std::mutex> lock(g_ctrl);
        if (!g_on.load()) return;
        g_on.store(false, std::memory_order_release); // new streams go to the game's compressor from now on
        if (GetTickCount64() - g_lastFastCountTick.load() > kPairGraceMs) RemoveLayerLocked();
    }
    const Stats s = GetStats();
    LOG_INFO(std::format("[FastRefPack] Off ({} streams, {} -> {} bytes, {} checked, {} different, {} did not fit){}", s.streams, s.bytesIn, s.bytesOut, s.checked, s.mismatches,
                         s.overflows, s.installed ? "; the layer is removed once no counted stream can still be waiting for its write" : ""));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void Tick() {
    RetireIdleStorage();
    if (g_on.load(std::memory_order_relaxed)) return;
    if (GetTickCount64() - g_lastFastCountTick.load(std::memory_order_relaxed) <= kPairGraceMs) return;
    if (!g_installed.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_on.load() && g_installed.load()) {
        RemoveLayerLocked();
        LOG_INFO("[FastRefPack] Layer removed");
    }
}

void SetVerifyEvery(int n) { g_verifyEvery.store(n < 0 ? 0 : n); }
int VerifyEvery() { return g_verifyEvery.load(); }
void SetCompareEvery(int n) { g_compareEvery.store(n < 0 ? 0 : n); }
int CompareEvery() { return g_compareEvery.load(); }
void SetChainDepth(int n) { g_chain.store(n < 4 ? 4 : (n > 256 ? 256 : n)); }
int ChainDepth() { return g_chain.load(); }

Stats GetStats() {
    Stats s;
    s.streams = c_streams.load();
    s.bytesIn = c_in.load();
    s.bytesOut = c_out.load();
    s.countingRuns = c_count.load();
    s.ms = static_cast<double>(g_ticks.load()) * g_qpcMs;
    s.countingMs = static_cast<double>(g_countTicks.load()) * g_qpcMs;
    s.passedThrough = c_passed.load();
    s.paired = c_paired.load();
    s.overflows = c_overflow.load();
    s.tempContexts = c_temp.load();
    s.reused = c_reused.load();
    s.reuseMissed = c_reuseMissed.load();
    s.large = c_large.load();
    s.largeParallel = c_largeParallel.load();
    s.largeBusy = c_largeBusy.load();
    s.pieces = c_pieces.load();
    s.piecesByWorkers = c_piecesByWorkers.load();
    s.checked = c_checked.load();
    s.mismatches = c_mismatch.load();
    s.notCheckable = c_notCheckable.load();
    s.compared = c_compared.load();
    s.comparedGameBytes = c_cmpGame.load();
    s.comparedFastBytes = c_cmpFast.load();
    s.comparedGameMs = static_cast<double>(g_cmpGameTicks.load()) * g_qpcMs;
    s.comparedFastMs = static_cast<double>(g_cmpFastTicks.load()) * g_qpcMs;
    s.selfDisabled = g_selfDisabled.load();
    s.gameDecoder = g_decoder != nullptr;
    s.installed = g_installed.load();
    std::lock_guard<std::mutex> lock(g_mismatchLock);
    s.lastMismatch = g_lastMismatch;
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off: a check found a stream the game could not read back (see ApexRadiance_LOG.txt)";
    if (!s.streams) return "On (nothing compressed yet)";
    std::string t = std::format("On: {} streams, {:.1f} MB -> {:.1f} MB ({:.0f}%) in {:.0f} ms; {} checked by decompressing, all equal", s.streams, static_cast<double>(s.bytesIn) / 1048576.0,
                                static_cast<double>(s.bytesOut) / 1048576.0, s.bytesIn ? 100.0 * static_cast<double>(s.bytesOut) / static_cast<double>(s.bytesIn) : 0.0, s.ms,
                                s.checked);
    if (s.compared && s.comparedGameBytes && s.comparedFastMs > 0.0)
        t += std::format("; vs the game on {}: size {:+.1f}%, {:.1f}x faster", s.compared,
                         100.0 * (static_cast<double>(s.comparedFastBytes) / static_cast<double>(s.comparedGameBytes) - 1.0), s.comparedGameMs / s.comparedFastMs);
    return t;
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    int every = g_verifyEvery.load();
    if (ApexUi::DiagnosticIntRow("Check 1 stream in N by decompressing##FrVerify", &every, 0, 64)) SetVerifyEvery(every);
    int cmp = g_compareEvery.load();
    if (ApexUi::DiagnosticIntRow("Also run the game's compressor on 1 stream in N (0 = never; adds its time)##FrCompare", &cmp, 0, 64)) SetCompareEvery(cmp);
    int chain = g_chain.load();
    if (ApexUi::DiagnosticIntRow("Search depth (candidates per position)##FrChain", &chain, 4, 256)) SetChainDepth(chain);
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
    if (ApexUi::BeginAdvanced("LiveCounters", "Live counters")) {
    ImGui::TextWrapped("%s", ("Faster cache compression: " + StatusText()).c_str());
    ImGui::TextWrapped("Streams %llu (%.1f ms), counting runs %llu (%.1f ms), writes after our counting run %llu; game's compressor %llu; did not fit (-1) %llu; temporary contexts %llu",
                        static_cast<unsigned long long>(s.streams), s.ms, static_cast<unsigned long long>(s.countingRuns), s.countingMs, static_cast<unsigned long long>(s.paired),
                        static_cast<unsigned long long>(s.passedThrough), static_cast<unsigned long long>(s.overflows), static_cast<unsigned long long>(s.tempContexts));
    ImGui::TextWrapped("Writes that copied their counting run's stream %llu (source changed since: %llu%s); large streams %llu, on %u worker threads %llu (pool busy: "
                        "%llu), pieces %llu, by the workers %llu",
                        static_cast<unsigned long long>(s.reused), static_cast<unsigned long long>(s.reuseMissed), CpuHasCrc32() ? "" : "; no SSE4.2, never copied",
                        static_cast<unsigned long long>(s.large), g_workersMade.load(), static_cast<unsigned long long>(s.largeParallel),
                        static_cast<unsigned long long>(s.largeBusy), static_cast<unsigned long long>(s.pieces), static_cast<unsigned long long>(s.piecesByWorkers));
    ImGui::TextWrapped("Checks (%s): %llu equal, %llu different, %llu not checked (no memory); layer %s", s.gameDecoder ? "game's decoder" : "Apex's copy of the decoder",
                        static_cast<unsigned long long>(s.checked), static_cast<unsigned long long>(s.mismatches), static_cast<unsigned long long>(s.notCheckable),
                        s.installed ? "installed" : "not installed");
    if (s.compared)
        ImGui::TextWrapped("Compared with the game's compressor on %llu streams: game %llu bytes in %.1f ms, Apex %llu bytes in %.1f ms", static_cast<unsigned long long>(s.compared),
                            static_cast<unsigned long long>(s.comparedGameBytes), s.comparedGameMs, static_cast<unsigned long long>(s.comparedFastBytes), s.comparedFastMs);
        ApexUi::EndAdvanced();
    }
}


void SaveDeveloperState(toml::table& out) {
    out.insert("verify_every", g_verifyEvery.load());
    out.insert("compare_every", g_compareEvery.load());
    out.insert("chain_depth", g_chain.load());
}
void LoadDeveloperState(const toml::table& t) {
    if (auto n = t["verify_every"].value<int64_t>()) { const int v = static_cast<int>(*n); g_verifyEvery.store(std::clamp(v, 0, 1024)); }
    if (auto n = t["compare_every"].value<int64_t>()) { const int v = static_cast<int>(*n); g_compareEvery.store(std::clamp(v, 0, 1024)); }
    if (auto n = t["chain_depth"].value<int64_t>()) { const int v = static_cast<int>(*n); g_chain.store(std::clamp(v, 4, 256)); }
}
} // namespace FastRefPack

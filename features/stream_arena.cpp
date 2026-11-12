// Stream arena (see stream_arena.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "stream_arena.h"
#include "stream_arena_sites.h"
#include "dev_switches.h"
#include "apex_log.h"
#include "memory_patch.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

namespace StreamArena {
namespace {

constexpr uintptr_t kCtor = 0x004DEBE0, kRelease = 0x004DEA80, kRealloc = 0x004DEE40, kWrite = 0x004DEFF0;
constexpr uintptr_t kCtorAt = kCtor + 0x2B, kReleaseAt = kRelease + 0x19, kReallocAt = kRealloc + 0xB9;
constexpr uintptr_t kReallocFreeRet = kReallocAt + 6;        // StubFree's return address from Realloc's free site
constexpr uintptr_t kReallocCopyAt = kRealloc + 0x8B;        // Realloc's call memcpy (0x004DEECB)
constexpr uintptr_t kWriteReallocAt = kWrite + 0x6C;         // Write's call Realloc (0x004DF05C)
constexpr uint32_t kTextWriter = 0x004B590C;                 // Write's return address in the text encoder
constexpr uintptr_t kComp = 0x004EC0A0;                      // the RefPack compressor (Scratch)
constexpr uintptr_t kScratchName = 0x00FB9030;               // its allocation name
constexpr uintptr_t kPkgFn[3] = {0x004A7030, 0x004A82C0, 0x004A8910};
constexpr uintptr_t kAsm = 0x006165A0, kAsmAllocAt = kAsm + 0x408, kAsmFreeAt = kAsm + 0x4F3; // 0x006169A8, 0x00616A93
constexpr uintptr_t kHeapMemalign = 0x004E6A20, kHeapFree = 0x004E5920;

constexpr uint32_t kSlice = 0x10000, kSlices = 768; // 48 MB
constexpr uint32_t kMinBytes = 128u << 10, kMaxBytes = 8u << 20;
constexpr DWORD kIdleMs = 5000, kUpkeepMs = 500, kLogMs = 120000;
constexpr BYTE kPoison = 0xB5, kPattern = 0xA7; // Verify: a freed block / Verify 2: a block handed out
enum : uint8_t { kFree = 0, kUsed = 1, kPurging = 2 };
enum Kind { kStream, kScratch, kPackage, kAssemble, kKinds };

SRWLOCK g_lock = SRWLOCK_INIT;
uint8_t g_state[kSlices];
bool g_committed[kSlices];
bool g_poisoned[kSlices]; // a free slice filled with kPoison (Verify)
uint16_t g_head[kSlices]; // slices of the block starting here (0: not a block's first slice)
uint8_t g_keep[kSlices];  // grown in place: Realloc's free of the old SharedPointer is not a free
uint32_t g_req[kSlices];  // bytes asked for
DWORD g_freedAt[kSlices];
std::atomic<uint8_t*> g_base{nullptr}; // nullptr: no arena (never made, or released by Retire)
std::atomic<bool> g_armed{false};      // new blocks may come from the arena
std::atomic<bool> g_grow{false};
int g_verify = 0;                      // 0, 1 (poison freed blocks) or 2 (also count bytes never written)
bool g_retiring = false;               // under g_lock
uint32_t g_live = 0;                   // under g_lock
std::atomic<uint64_t> c_served[kKinds], c_commits{0}, c_fallback{0}, c_commitFail{0}, c_doubleFree{0}, c_decommitKB{0};
std::atomic<uint64_t> c_requests{0}, c_raised{0}, c_inPlace{0}, c_inPlaceBlocked{0}, c_notCopiedKB{0};
std::atomic<uint64_t> c_uafw{0}, c_unwrittenBlocks{0}, c_unwrittenKB{0};
std::atomic<uint64_t> g_ownTicks{0}, g_zeroTicks{0};
double g_qpcMs = 0.0;

uint64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

bool Owned(const void* p) {
    const uint8_t* b = g_base.load(std::memory_order_acquire);
    return b && p >= b && p < b + uint64_t(kSlices) * kSlice;
}

bool AllIs(const uint8_t* p, size_t n, BYTE v) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != v) return false;
    return true;
}

// The game's allocator (ICoreAllocator: vtable +8 Alloc(size, name, flags), +0xC Free(ptr, size); thiscall)
void* GameAlloc(void* a, DWORD size, const char* name) {
    return reinterpret_cast<void*(__thiscall*)(void*, DWORD, const char*, DWORD)>((*reinterpret_cast<void***>(a))[2])(a, size, name, 0);
}
void GameFree(void* a, void* p) { reinterpret_cast<void(__thiscall*)(void*, void*, DWORD)>((*reinterpret_cast<void***>(a))[3])(a, p, 0); }

// Slices [s, s + n) back to free; lock held
void FreeRun(uint32_t s, uint32_t n, DWORD now, bool poisoned) {
    for (uint32_t j = s; j < s + n; j++) g_state[j] = kFree, g_freedAt[j] = now, g_poisoned[j] = poisoned;
}

// The bytes [from, to) of a block that hold an older block's data (the slices marked dirty) or the bytes of fresh pages
// (Verify 2 only) get the hand-out value. Poisoned slices are checked first: a byte that changed was written after free.
void Prepare(uint8_t* p, uint32_t s, uint32_t from, uint32_t to, const bool* dirty, const bool* poisoned) {
    const uint64_t t0 = Qpc();
    for (uint32_t j = from / kSlice; j * kSlice < to; j++) {
        const uint32_t lo = (std::max)(from, j * kSlice), hi = (std::min)(to, (j + 1) * kSlice);
        if (poisoned[j - s] && !AllIs(p + j * kSlice - s * kSlice, kSlice, kPoison) && c_uafw.fetch_add(1) < 8)
            LOG_WARNING(std::format("[StreamArena] Verify: a freed block was written after its free (slice {:#010x})", reinterpret_cast<uintptr_t>(p + (j - s) * kSlice)));
        if (g_verify == 2) std::memset(p + lo - s * kSlice, kPattern, hi - lo);
        else if (dirty[j - s]) std::memset(p + lo - s * kSlice, 0, hi - lo); // fresh pages are zero already
    }
    g_zeroTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
}

void* Take(DWORD size, Kind kind) {
    const uint64_t t0 = Qpc();
    const uint32_t n = (size + kSlice - 1) / kSlice;
    bool dirty[kMaxBytes / kSlice], poisoned[kMaxBytes / kSlice];
    bool commit = false;
    uint32_t best = kSlices, bestLen = kSlices + 1;
    AcquireSRWLockExclusive(&g_lock);
    uint8_t* const base = g_base.load(std::memory_order_relaxed);
    if (!base || !g_armed.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&g_lock);
        return nullptr;
    }
    for (uint32_t i = 0; i < kSlices;) { // best fit over the free runs
        if (g_state[i] != kFree) { i++; continue; }
        const uint32_t s = i;
        while (i < kSlices && g_state[i] == kFree) i++;
        if (i - s >= n && i - s < bestLen) best = s, bestLen = i - s;
    }
    if (best == kSlices) {
        ReleaseSRWLockExclusive(&g_lock);
        c_fallback.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    for (uint32_t j = 0; j < n; j++) {
        g_state[best + j] = kUsed;
        dirty[j] = g_committed[best + j];
        poisoned[j] = g_poisoned[best + j];
        g_poisoned[best + j] = false;
        commit |= !dirty[j];
        g_committed[best + j] = true;
    }
    g_head[best] = static_cast<uint16_t>(n);
    g_keep[best] = 0;
    g_req[best] = size;
    g_live++;
    ReleaseSRWLockExclusive(&g_lock);
    uint8_t* const p = base + best * kSlice;
    if (commit) {
        c_commits.fetch_add(1, std::memory_order_relaxed);
        if (!VirtualAlloc(p, n * kSlice, MEM_COMMIT, PAGE_READWRITE)) { // commit limit: the game's call
            AcquireSRWLockExclusive(&g_lock);
            for (uint32_t j = 0; j < n; j++) g_committed[best + j] = dirty[j], g_poisoned[best + j] = poisoned[j];
            g_head[best] = 0;
            g_live--;
            for (uint32_t j = best; j < best + n; j++) g_state[j] = kFree; // their idle time stays
            ReleaseSRWLockExclusive(&g_lock);
            c_commitFail.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    // A reused slice holds an older block's bytes. Consumers accept a short read (the loader) or read before they
    // write, so the buffer is handed out zero-filled, as the game's own fresh block is
    Prepare(p, best, best * kSlice, best * kSlice + size, dirty, poisoned);
    c_served[kind].fetch_add(1, std::memory_order_relaxed);
    g_ownTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
    return p;
}

void Put(uint8_t* p) {
    const uint64_t t0 = Qpc();
    const uint32_t s = static_cast<uint32_t>(p - g_base.load(std::memory_order_relaxed)) / kSlice;
    AcquireSRWLockExclusive(&g_lock);
    if ((reinterpret_cast<uintptr_t>(p) & (kSlice - 1)) || g_state[s] != kUsed || !g_head[s]) {
        ReleaseSRWLockExclusive(&g_lock);
        c_doubleFree.fetch_add(1, std::memory_order_relaxed); // never handed to the game's free
        return;
    }
    const uint32_t n = g_head[s], req = g_req[s];
    g_head[s] = 0; // in transit: still used, a second free is counted
    g_keep[s] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (g_verify == 2) {
        uint32_t left = 0;
        for (uint32_t i = 0; i + 8 <= req; i += 8) left += AllIs(p + i, 8, kPattern);
        if (left) c_unwrittenBlocks.fetch_add(1, std::memory_order_relaxed), c_unwrittenKB.fetch_add(left * 8 / 1024, std::memory_order_relaxed);
    }
    if (g_verify) std::memset(p, kPoison, n * kSlice);
    AcquireSRWLockExclusive(&g_lock);
    FreeRun(s, n, GetTickCount(), g_verify != 0);
    g_live--;
    ReleaseSRWLockExclusive(&g_lock);
    g_ownTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
}

// Realloc of a stream whose buffer is an arena block that this Realloc will free (its SharedPointer has one reference,
// owns its data, same allocator): the block takes the free slices behind it and keeps its address. Realloc then copies
// the block onto itself (GrowCopy does nothing) and its free of the old SharedPointer clears the mark (StubFree).
void* GrowInPlace(void* stream, DWORD size, void* allocator) {
    const auto* st = static_cast<const uint8_t*>(stream);
    const auto* sp = *reinterpret_cast<const uint8_t* const*>(st + 4);
    if (!sp) return nullptr;
    uint8_t* const p = *reinterpret_cast<uint8_t* const*>(sp + 8);
    const uint32_t oldCap = *reinterpret_cast<const uint32_t*>(st + 0x14);
    if (!Owned(p) || *reinterpret_cast<void* const*>(sp + 4) != allocator || *reinterpret_cast<const int32_t*>(sp + 0xC) != 1 || !sp[0x10] || size <= oldCap)
        return nullptr;
    const uint32_t m = (size + kSlice - 1) / kSlice;
    bool dirty[kMaxBytes / kSlice] = {}, poisoned[kMaxBytes / kSlice] = {};
    bool commit = false;
    AcquireSRWLockExclusive(&g_lock);
    uint8_t* const base = g_base.load(std::memory_order_relaxed);
    const uint32_t s = static_cast<uint32_t>(p - base) / kSlice, n = g_head[s];
    bool ok = !(reinterpret_cast<uintptr_t>(p) & (kSlice - 1)) && g_state[s] == kUsed && n && !g_keep[s] && g_armed.load(std::memory_order_relaxed) &&
              s + m <= kSlices && oldCap <= n * kSlice;
    for (uint32_t j = s + n; ok && j < s + m; j++) ok = g_state[j] == kFree;
    if (!ok) {
        ReleaseSRWLockExclusive(&g_lock);
        c_inPlaceBlocked.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    for (uint32_t j = s; j < s + m; j++) dirty[j - s] = true; // the old slices hold the old block's tail past oldCap
    for (uint32_t j = s + n; j < s + m; j++) {
        g_state[j] = kUsed;
        dirty[j - s] = g_committed[j];
        poisoned[j - s] = g_poisoned[j];
        g_poisoned[j] = false;
        commit |= !g_committed[j];
        g_committed[j] = true;
    }
    g_head[s] = static_cast<uint16_t>(m);
    g_keep[s] = 1;
    const uint32_t oldReq = g_req[s];
    g_req[s] = size;
    ReleaseSRWLockExclusive(&g_lock);
    if (commit && !VirtualAlloc(p + n * kSlice, (m - n) * kSlice, MEM_COMMIT, PAGE_READWRITE)) {
        AcquireSRWLockExclusive(&g_lock);
        for (uint32_t j = s + n; j < s + m; j++) g_state[j] = kFree, g_committed[j] = dirty[j - s], g_poisoned[j] = poisoned[j - s];
        g_head[s] = static_cast<uint16_t>(n);
        g_keep[s] = 0;
        g_req[s] = oldReq;
        ReleaseSRWLockExclusive(&g_lock);
        c_commitFail.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    Prepare(p, s, s * kSlice + oldCap, s * kSlice + size, dirty, poisoned); // the new bytes, as in a fresh block
    c_inPlace.fetch_add(1, std::memory_order_relaxed);
    c_notCopiedKB.fetch_add(oldCap >> 10, std::memory_order_relaxed);
    return p;
}

// In place of the allocator call in the SharedPointer constructor (reached only from MemoryStream::Realloc)
void* __stdcall StubAlloc(DWORD size, void* stream, void* allocator, const char* name) {
    c_requests.fetch_add(1, std::memory_order_relaxed);
    if (size >= kMinBytes && size <= kMaxBytes && g_armed.load(std::memory_order_relaxed)) {
        if (g_grow.load(std::memory_order_relaxed) && stream)
            if (void* p = GrowInPlace(stream, size, allocator)) return p;
        if (void* p = Take(size, kStream)) return p;
    }
    return GameAlloc(allocator, size, name);
}

// A block grown in place, freed by Realloc as the old SharedPointer's data: the mark goes, the block stays
bool ClearKeep(const uint8_t* p) {
    const uint32_t s = static_cast<uint32_t>(p - g_base.load(std::memory_order_relaxed)) / kSlice;
    bool kept = false;
    AcquireSRWLockExclusive(&g_lock);
    if (!(reinterpret_cast<uintptr_t>(p) & (kSlice - 1)) && g_state[s] == kUsed && g_head[s] && g_keep[s]) g_keep[s] = 0, kept = true;
    ReleaseSRWLockExclusive(&g_lock);
    return kept;
}

// In place of allocator->Free(data, 0); sp = the SharedPointer (+4 allocator, +8 data)
void __stdcall StubFree(void* sp) {
    void* const allocator = static_cast<void* const*>(sp)[1];
    uint8_t* const p = static_cast<uint8_t* const*>(sp)[2];
    if (!Owned(p)) return GameFree(allocator, p);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == kReallocFreeRet && ClearKeep(p)) return;
    Put(p);
}

// Grow: the capacity Write asks Realloc for, raised for small streams that already have a buffer (the game grows 1.5x
// from the first write's size: a stream fed four bytes at a time reallocates ten times before 256 bytes). Never lowered;
// nothing from kMinBytes on (there the arena grows the block in place instead).
DWORD GrowCap(const void* stream, DWORD want, bool text) {
    if (!g_grow.load(std::memory_order_relaxed) || want >= kMinBytes) return want;
    const auto* st = static_cast<const uint8_t*>(stream);
    const auto* sp = *reinterpret_cast<const uint8_t* const*>(st + 4);
    if (!sp || !*reinterpret_cast<void* const*>(sp + 8)) return want; // a stream's first buffer: as asked
    const uint32_t old = *reinterpret_cast<const uint32_t*>(st + 0x14);
    uint32_t c = (std::max)(old < 4096 ? old * 4 : old * 2, 64u);
    if (text) c = (std::max)(c, 4096u);
    if (c <= want || c >= kMinBytes) return want;
    c_raised.fetch_add(1, std::memory_order_relaxed);
    return c;
}

// In place of Write's call of Realloc (thiscall, capacity, ret 4). Write's frame above the argument: edi, ebx, esi, ebp,
// 8 bytes of locals, then Write's return address (its head is byte-checked)
bool __fastcall WriteGrow(void* stream, void*, DWORD capacity) {
    const uint32_t writer = static_cast<const uint32_t*>(_AddressOfReturnAddress())[8];
    return reinterpret_cast<bool(__thiscall*)(void*, DWORD)>(kRealloc)(stream, GrowCap(stream, capacity, writer == kTextWriter));
}

// In place of Realloc's call memcpy (cdecl): a block grown in place is its own copy
void* __cdecl GrowCopy(void* dst, const void* src, size_t n) { return dst == src ? dst : std::memcpy(dst, src, n); }

void* SiteTake(DWORD size, Kind kind) {
    return size >= kMinBytes && size <= kMaxBytes && g_armed.load(std::memory_order_relaxed) ? Take(size, kind) : nullptr;
}
// The compressor's two work tables (Scratch) and the package writer's two record buffers (Package)
void* __stdcall ScratchAlloc(DWORD size, void* allocator) {
    if (void* p = SiteTake(size, kScratch)) return p;
    return GameAlloc(allocator, size, reinterpret_cast<const char*>(kScratchName));
}
void* __stdcall PackageAlloc(DWORD size, void* allocator, const char* name) {
    if (void* p = SiteTake(size, kPackage)) return p;
    return GameAlloc(allocator, size, name);
}
void __stdcall SiteFree(void* p, void* allocator) {
    if (Owned(p)) Put(static_cast<uint8_t*>(p));
    else GameFree(allocator, p);
}
// The block assembler's scratch buffer: Memalign(size, 16, 0, 0) and Free on the MemoryMan heap (thiscall). An arena
// block starts on a 64 KB boundary
void* __fastcall AssembleAlloc(void* heap, void*, DWORD size, DWORD align, DWORD a3, DWORD a4) {
    if (align == 16 && !a3 && !a4)
        if (void* p = SiteTake(size, kAssemble)) return p;
    return reinterpret_cast<void*(__thiscall*)(void*, DWORD, DWORD, DWORD, DWORD)>(kHeapMemalign)(heap, size, align, a3, a4);
}
void __fastcall AssembleFree(void* heap, void*, void* p) {
    if (Owned(p)) Put(static_cast<uint8_t*>(p));
    else reinterpret_cast<void(__thiscall*)(void*, void*)>(kHeapFree)(heap, p);
}

// Free committed slices idle for idleMs are decommitted. Returns KB.
uint32_t Decommit(DWORD idleMs) {
    uint32_t runs[kSlices][2], nRuns = 0;
    const DWORD now = GetTickCount();
    AcquireSRWLockExclusive(&g_lock);
    uint8_t* const base = g_base.load(std::memory_order_relaxed);
    for (uint32_t i = 0; base && i < kSlices;) {
        const auto idle = [&](uint32_t j) { return g_state[j] == kFree && g_committed[j] && now - g_freedAt[j] >= idleMs; };
        if (!idle(i)) { i++; continue; }
        const uint32_t s = i;
        while (i < kSlices && idle(i)) g_state[i++] = kPurging;
        runs[nRuns][0] = s, runs[nRuns][1] = i - s, nRuns++;
    }
    ReleaseSRWLockExclusive(&g_lock);
    uint32_t kb = 0;
    for (uint32_t r = 0; r < nRuns; r++) {
        const bool ok = VirtualFree(base + runs[r][0] * kSlice, runs[r][1] * kSlice, MEM_DECOMMIT) != FALSE;
        AcquireSRWLockExclusive(&g_lock);
        for (uint32_t j = runs[r][0]; j < runs[r][0] + runs[r][1]; j++) {
            g_state[j] = kFree, g_committed[j] = !ok;
            if (ok) g_poisoned[j] = false; // zero pages from now on
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (ok) kb += runs[r][1] * (kSlice >> 10);
    }
    c_decommitKB.fetch_add(kb, std::memory_order_relaxed);
    return kb;
}

// Retire: once no block is in use, the range stops being "ours" (under the lock, so no free can be mid-way) and goes back
bool ReleaseIfRetired() {
    AcquireSRWLockExclusive(&g_lock);
    bool purging = false;
    for (uint8_t st : g_state) purging |= st == kPurging; // a save's decommit still running on that range
    uint8_t* const base = g_retiring && !g_live && !purging ? g_base.exchange(nullptr) : nullptr;
    if (base) std::memset(g_committed, 0, sizeof g_committed);
    ReleaseSRWLockExclusive(&g_lock);
    if (!base) return false;
    VirtualFree(base, 0, MEM_RELEASE);
    LOG_INFO("[StreamArena] Address space tight: the 48 MB arena was given back (" + StatusText() + ")");
    return true;
}

DWORD WINAPI Upkeep(LPVOID) {
    uint64_t lastServed = 0;
    for (DWORD lastLog = GetTickCount();;) {
        Sleep(kUpkeepMs);
        Decommit(kIdleMs);
        if (ReleaseIfRetired()) return 0;
        const uint64_t served = c_served[kStream].load() + c_served[kScratch].load() + c_served[kPackage].load() + c_served[kAssemble].load();
        if (GetTickCount() - lastLog >= kLogMs && served != lastServed) {
            lastLog = GetTickCount(), lastServed = served;
            LOG_INFO("[StreamArena] " + StatusText());
        }
    }
}

bool Matches(uintptr_t at, const BYTE* bytes, size_t n, const char* what) {
    if (MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(at), bytes, n)) return true;
    LOG_INFO(std::format("[StreamArena] {} at {:#010x} differs from Steam 1.67.2: not installed", what, at));
    return false;
}

// call rel32 at `at` to `to`, the CALL ending where the original ended
void Call(BYTE* b, uintptr_t at, const void* to) {
    b[0] = 0xE8;
    const int32_t r = MemPatch::CalculateRelativeOffset(at, reinterpret_cast<uintptr_t>(to), 5);
    std::memcpy(b + 1, &r, 4);
}

bool InstallSites() {
    if (!Matches(kCtor, kCtorBytes, sizeof kCtorBytes, "The SharedPointer constructor") || !Matches(kRelease, kReleaseBytes, sizeof kReleaseBytes, "SharedPointer::Release") ||
        !Matches(kRealloc, kReallocBytes, sizeof kReallocBytes, "MemoryStream::Realloc"))
        return false;
    static BYTE freeRel[16], freeRealloc[16], alloc[12];
    const auto freeSite = [](BYTE* b, uintptr_t at) { // push esi; call StubFree; jmp +8; int3 x 8
        b[0] = 0x56;
        Call(b + 1, at + 1, reinterpret_cast<const void*>(&StubFree));
        b[6] = 0xEB, b[7] = 0x08, std::memset(b + 8, 0xCC, 8);
    };
    freeSite(freeRel, kReleaseAt);
    freeSite(freeRealloc, kReallocAt);
    // push ecx (name); push eax (allocator); push ebx (the stream: Realloc's this); push [esp+14h] (size); call StubAlloc.
    // Four arguments, stdcall: the stack is as after the original three-argument thiscall, and the return stays 0x004DEC17
    constexpr BYTE head[7] = {0x51, 0x50, 0x53, 0xFF, 0x74, 0x24, 0x14};
    std::memcpy(alloc, head, 7);
    Call(alloc + 7, kCtorAt + 7, reinterpret_cast<const void*>(&StubAlloc));
    const MemPatch::CodeWrite writes[] = {{kReleaseAt, freeRel, 16, 16}, {kReallocAt, freeRealloc, 16, 16}, {kCtorAt, alloc, 12, 12}};
    if (MemPatch::WriteCodeBatchSuspended(writes, 3)) return true;
    // a write failed: the earlier ones stay and pass through (nothing is armed, so no arena block exists)
    LOG_WARNING("[StreamArena] A stream site could not be written: off");
    return false;
}

// Grow: Write's call Realloc (0x004DF05C) and Realloc's call memcpy (0x004DEECB), each a 5-byte call whose target changes
bool InstallGrow() {
    if (!Matches(kWrite, kWriteBytes, sizeof kWriteBytes, "MemoryStream::Write") ||
        !Matches(kReallocCopyAt, kReallocBytes + 0x8B, 5, "Realloc's copy"))
        return false;
    static BYTE write[5], copy[5];
    Call(write, kWriteReallocAt, reinterpret_cast<const void*>(&WriteGrow));
    Call(copy, kReallocCopyAt, reinterpret_cast<const void*>(&GrowCopy));
    const MemPatch::CodeWrite writes[] = {{kReallocCopyAt, copy, 5, 5}, {kWriteReallocAt, write, 5, 5}};
    return MemPatch::WriteCodeBatchSuspended(writes, 2);
}

// Scratch: the compressor's two table frees, then its two allocations. Each new call ends where the old one ended
// (returns 0x004EC122 / 0x004EC140 kept).
bool InstallScratch() {
    if (!Matches(kComp, kCompBytes, sizeof kCompBytes, "The RefPack compressor")) return false;
    constexpr BYTE nop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    static BYTE f1[12], f2[12], a1[16], a2[19];
    // mov eax,[edi]; mov edx,[eax+0Ch]; push 0; push ecx; mov ecx,edi; call edx -> push edi (allocator); push ecx (the
    // table); call SiteFree; nop
    for (auto [b, at] : {std::pair{f1, kComp + 0x119}, std::pair{f2, kComp + 0x12E}}) {
        b[0] = 0x57, b[1] = 0x51;
        Call(b + 2, at + 2, reinterpret_cast<const void*>(&SiteFree));
        std::memcpy(b + 7, nop5, 5);
    }
    // push 0; push name; push 40000h; mov ecx,edi; call edx -> push edi; push 40000h; nop; call ScratchAlloc
    constexpr BYTE a1head[6] = {0x57, 0x68, 0x00, 0x00, 0x04, 0x00};
    std::memcpy(a1, a1head, 6), std::memcpy(a1 + 6, nop5, 5);
    Call(a1 + 11, kComp + 0x72 + 11, reinterpret_cast<const void*>(&ScratchAlloc));
    // push 0; lea edx,[ecx*4+4]; push name; push edx; mov ecx,edi; call eax -> lea edx,[ecx*4+4]; push edi; push edx; nop;
    // call ScratchAlloc
    std::memcpy(a2, kCompBytes + 0x8D + 2, 7), a2[7] = 0x57, a2[8] = 0x52, std::memcpy(a2 + 9, nop5, 5);
    Call(a2 + 14, kComp + 0x8D + 14, reinterpret_cast<const void*>(&ScratchAlloc));
    const MemPatch::CodeWrite writes[] = {{kComp + 0x119, f1, 12, 12}, {kComp + 0x12E, f2, 12, 12}, {kComp + 0x72, a1, 16, 16}, {kComp + 0x8D, a2, 19, 19}};
    return MemPatch::WriteCodeBatchSuspended(writes, 4);
}

// Package: the four frees then the two allocations of the record commit (byte layouts read from the three functions):
// the allocator stays in ecx, the pointer is loaded as before, then push allocator, push pointer, call SiteFree; the
// allocations push name, allocator, size and call PackageAlloc, ending where the game's call ended (0x004A7216 / 0x004A89F7)
bool InstallPackage() {
    const BYTE* const fn[3] = {kPkgBytes0, kPkgBytes1, kPkgBytes2};
    const size_t len[3] = {sizeof kPkgBytes0, sizeof kPkgBytes1, sizeof kPkgBytes2};
    for (int i = 0; i < 3; i++)
        if (!Matches(kPkgFn[i], fn[i], len[i], "The package writer")) return false;
    struct Site {
        int fn;
        uint32_t at;
        uint8_t n, call;
        BYTE b[20];
    };
    static Site sites[6] = {
        {0, 0x275, 13, 5, {0x8B, 0x4B, 0x10, 0x51, 0x50, 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x00}},                                  // 0x004A72A5
        {1, 0x08E, 13, 5, {0x8B, 0x4E, 0x10, 0x51, 0x50, 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x00}},                                  // 0x004A834E
        {2, 0x1D1, 16, 9, {0x8B, 0x4E, 0x10, 0x8B, 0x44, 0x24, 0x18, 0x51, 0x50, 0xE8, 0, 0, 0, 0, 0x66, 0x90}},                // 0x004A8AE1
        {2, 0x1E1, 16, 9, {0x8B, 0x4E, 0x10, 0x8B, 0x54, 0x24, 0x1C, 0x51, 0x52, 0xE8, 0, 0, 0, 0, 0x66, 0x90}},                // 0x004A8AF1
        {0, 0x1D4, 18, 13, {0x8B, 0x4B, 0x10, 0x68, 0x7C, 0x25, 0xFB, 0x00, 0x51, 0x50, 0x0F, 0x1F, 0x00, 0xE8}},               // 0x004A7204
        {2, 0x0D3, 20, 15, {0x8B, 0x4E, 0x10, 0x8D, 0x47, 0x10, 0x68, 0xA4, 0x26, 0xFB, 0x00, 0x51, 0x50, 0x66, 0x90, 0xE8}}};  // 0x004A89E3
    MemPatch::CodeWrite writes[6];
    for (int i = 0; i < 6; i++) {
        Site& k = sites[i];
        const uintptr_t at = kPkgFn[k.fn] + k.at;
        Call(k.b + k.call, at + k.call, i < 4 ? reinterpret_cast<const void*>(&SiteFree) : reinterpret_cast<const void*>(&PackageAlloc));
        writes[i] = {at, k.b, k.n, k.n};
    }
    return MemPatch::WriteCodeBatchSuspended(writes, 6);
}

// Assemble: the free's call target, then the allocation's (both stay 5-byte calls; return 0x006169AD kept)
bool InstallAssemble() {
    if (!Matches(kAsm, kAsmBytes, sizeof kAsmBytes, "The block assembler")) return false;
    static BYTE f[5], a[5];
    Call(f, kAsmFreeAt, reinterpret_cast<const void*>(&AssembleFree));
    Call(a, kAsmAllocAt, reinterpret_cast<const void*>(&AssembleAlloc));
    const MemPatch::CodeWrite writes[] = {{kAsmFreeAt, f, 5, 5}, {kAsmAllocAt, a, 5, 5}};
    return MemPatch::WriteCodeBatchSuspended(writes, 2);
}

const char* State(bool asked, bool on) { return !asked ? "off" : on ? "on" : "not installed"; }

} // namespace

void Install() {
    if (!DevSwitches::StreamArena()) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    uint8_t* const base = static_cast<uint8_t*>(VirtualAlloc(nullptr, uint64_t(kSlices) * kSlice, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE));
    if (!base) {
        LOG_INFO("[StreamArena] No room for the 48 MB arena: off");
        return;
    }
    if (!InstallSites()) {
        VirtualFree(base, 0, MEM_RELEASE);
        return;
    }
    g_verify = DevSwitches::streamArenaVerify.load();
    g_base.store(base, std::memory_order_release);
    g_armed.store(true, std::memory_order_release);
    // the extra parts after the stream sites: their frees reach Put only for blocks the arena handed out
    const bool grow = DevSwitches::streamArenaGrow.load() && InstallGrow();
    g_grow.store(grow);
    const bool scratch = DevSwitches::streamArenaScratch.load() && InstallScratch();
    const bool package = DevSwitches::streamArenaPackage.load() && InstallPackage();
    const bool assemble = DevSwitches::streamArenaAssemble.load() && InstallAssemble();
    if (HANDLE t = CreateThread(nullptr, 0, Upkeep, nullptr, 0, nullptr)) CloseHandle(t);
    LOG_INFO(std::format("[StreamArena] On: buffers of {} KB to {} MB from one 48 MB arena at {:#010x}; grow {}, compressor tables {}, package writer {}, "
                         "block assembler {}, verify {}",
                         kMinBytes >> 10, kMaxBytes >> 20, reinterpret_cast<uintptr_t>(base), State(DevSwitches::streamArenaGrow.load(), grow),
                         State(DevSwitches::streamArenaScratch.load(), scratch), State(DevSwitches::streamArenaPackage.load(), package),
                         State(DevSwitches::streamArenaAssemble.load(), assemble), g_verify));
}

uint32_t PrepareForSave() {
    if (!g_base.load()) return 0;
    const uint32_t kb = Decommit(0);
    LOG_INFO("[StreamArena] Before the save: " + StatusText());
    return kb;
}

void Retire() {
    if (!g_base.load()) return;
    g_armed.store(false);
    AcquireSRWLockExclusive(&g_lock);
    g_retiring = true;
    ReleaseSRWLockExclusive(&g_lock);
}

std::string StatusText() {
    AcquireSRWLockShared(&g_lock);
    const uint32_t live = g_live;
    uint32_t committed = 0;
    for (bool c : g_committed) committed += c;
    ReleaseSRWLockShared(&g_lock);
    std::string s = std::format("served: streams {}, compressor {}, package {}, assembler {} (of {} stream requests); {} commit calls, {} fallbacks, "
                                "{} commit failures, {} double frees, {} MB decommitted; {} in use, {} MB committed; own time {:.1f} ms, of it filling {:.1f} ms",
                                c_served[kStream].load(), c_served[kScratch].load(), c_served[kPackage].load(), c_served[kAssemble].load(), c_requests.load(),
                                c_commits.load(), c_fallback.load(), c_commitFail.load(), c_doubleFree.load(), c_decommitKB.load() >> 10, live,
                                committed * (kSlice >> 10) >> 10, g_ownTicks.load() * g_qpcMs, g_zeroTicks.load() * g_qpcMs);
    if (g_grow.load())
        s += std::format("; grow: {} capacities raised, {} grown in place ({} MB not copied), {} not", c_raised.load(), c_inPlace.load(), c_notCopiedKB.load() >> 10,
                         c_inPlaceBlocked.load());
    if (g_verify)
        s += std::format("; verify {}: {} written after free, {} blocks with unwritten bytes ({} KB)", g_verify, c_uafw.load(), c_unwrittenBlocks.load(),
                         c_unwrittenKB.load());
    return s;
}

} // namespace StreamArena

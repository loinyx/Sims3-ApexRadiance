// Stream arena (see stream_arena.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "stream_arena.h"
#include "dev_switches.h"
#include "apex_log.h"
#include "memory_patch.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

namespace StreamArena {
namespace {

constexpr uintptr_t kCtor = 0x004DEBE0, kRelease = 0x004DEA80, kRealloc = 0x004DEE40;
constexpr uintptr_t kCtorAt = kCtor + 0x2B, kReleaseAt = kRelease + 0x19, kReallocAt = kRealloc + 0xB9;
constexpr BYTE kCtorBytes[0x4B] = { // 0x004DEBE0: SharedPointer(size, allocator, name)
    0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0, 0x56, 0x8B, 0xF1, 0xC7, 0x06, 0xD0, 0x84, 0xFB, 0x00, 0x75, 0x05, 0xE8, 0xDA, 0xAE, 0xFF, 0xFF, 0x8B, 0x4C,
    0x24, 0x10, 0x85, 0xC9, 0x89, 0x46, 0x04, 0x75, 0x05, 0xB9, 0xD4, 0x84, 0xFB, 0x00, 0x8B, 0x10, 0x8B, 0x52, 0x08, 0x6A, 0x00, 0x51, 0x8B, 0x4C,
    0x24, 0x10, 0x51, 0x8B, 0xC8, 0xFF, 0xD2, 0x89, 0x46, 0x08, 0xC7, 0x46, 0x0C, 0x00, 0x00, 0x00, 0x00, 0xC6, 0x46, 0x10, 0x01, 0x8B, 0xC6, 0x5E,
    0xC2, 0x0C, 0x00};
constexpr BYTE kReleaseBytes[0x37] = { // 0x004DEA80: SharedPointer::Release
    0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x0C, 0x83, 0xF8, 0x01, 0x7E, 0x08, 0x83, 0xC0, 0xFF, 0x89, 0x46, 0x0C, 0x5E, 0xC3, 0x80, 0x7E, 0x10, 0x00, 0x74,
    0x10, 0x8B, 0x4E, 0x04, 0x8B, 0x01, 0x8B, 0x56, 0x08, 0x8B, 0x40, 0x0C, 0x6A, 0x00, 0x52, 0xFF, 0xD0, 0x8B, 0x16, 0x8B, 0x02, 0x6A, 0x01, 0x8B,
    0xCE, 0xFF, 0xD0, 0x33, 0xC0, 0x5E, 0xC3};
constexpr BYTE kReallocBytes[0xE2] = { // 0x004DEE40: MemoryStream::Realloc
    0x53, 0x55, 0x8B, 0xD9, 0x8B, 0x43, 0x04, 0x85, 0xC0, 0x56, 0x57, 0x74, 0x07, 0x8B, 0x40, 0x04, 0x85, 0xC0, 0x75, 0x05, 0xE8, 0x77, 0xAC, 0xFF,
    0xFF, 0x8B, 0x6C, 0x24, 0x14, 0x85, 0xED, 0x8B, 0xF0, 0x74, 0x49, 0x8B, 0x43, 0x08, 0x85, 0xC0, 0x75, 0x05, 0xB8, 0xF8, 0x84, 0xFB, 0x00, 0x50,
    0x56, 0x6A, 0x14, 0xE8, 0x98, 0x16, 0x00, 0x00, 0x83, 0xC4, 0x0C, 0x85, 0xC0, 0x74, 0x24, 0x8B, 0x4B, 0x08, 0x51, 0x56, 0x55, 0x8B, 0xC8, 0xE8,
    0x54, 0xFD, 0xFF, 0xFF, 0x8B, 0xF8, 0x85, 0xFF, 0x74, 0x11, 0x83, 0x47, 0x0C, 0x01, 0x83, 0x7F, 0x08, 0x00, 0x75, 0x12, 0x8B, 0xCF, 0xE8, 0xDD,
    0xFB, 0xFF, 0xFF, 0x5F, 0x5E, 0x5D, 0x32, 0xC0, 0x5B, 0xC2, 0x04, 0x00, 0x33, 0xFF, 0x8B, 0x4B, 0x04, 0x85, 0xC9, 0x74, 0x5E, 0x85, 0xFF, 0x74,
    0x1A, 0x8B, 0x43, 0x14, 0x3B, 0xE8, 0x73, 0x02, 0x8B, 0xC5, 0x8B, 0x51, 0x08, 0x50, 0x8B, 0x47, 0x08, 0x52, 0x50, 0xE8, 0xD6, 0xF9, 0xA9, 0x00,
    0x83, 0xC4, 0x0C, 0x8B, 0x73, 0x04, 0x8B, 0x46, 0x0C, 0x83, 0xF8, 0x01, 0x7E, 0x15, 0x83, 0xC0, 0xFF, 0x89, 0x46, 0x0C, 0x89, 0x7B, 0x04, 0x5F,
    0x5E, 0x89, 0x6B, 0x14, 0x5D, 0xB0, 0x01, 0x5B, 0xC2, 0x04, 0x00, 0x80, 0x7E, 0x10, 0x00, 0x74, 0x10, 0x8B, 0x4E, 0x04, 0x8B, 0x11, 0x8B, 0x46,
    0x08, 0x8B, 0x52, 0x0C, 0x6A, 0x00, 0x50, 0xFF, 0xD2, 0x8B, 0x06, 0x8B, 0x10, 0x6A, 0x01, 0x8B, 0xCE, 0xFF, 0xD2, 0x89, 0x7B, 0x04, 0x5F, 0x5E,
    0x89, 0x6B, 0x14, 0x5D, 0xB0, 0x01, 0x5B, 0xC2, 0x04, 0x00};

constexpr uint32_t kSlice = 0x10000, kSlices = 768; // 48 MB
constexpr uint32_t kMinBytes = 128u << 10, kMaxBytes = 8u << 20;
constexpr DWORD kIdleMs = 5000, kUpkeepMs = 500;
enum : uint8_t { kFree = 0, kUsed = 1, kPurging = 2 };

SRWLOCK g_lock = SRWLOCK_INIT;
uint8_t g_state[kSlices];
bool g_committed[kSlices];
uint16_t g_head[kSlices]; // slices of the block starting here (0: not a block's first slice)
DWORD g_freedAt[kSlices];
std::atomic<uint8_t*> g_base{nullptr}; // nullptr: no arena (never made, or released by Retire)
std::atomic<bool> g_armed{false};      // new blocks may come from the arena
bool g_retiring = false;               // under g_lock
uint32_t g_live = 0;                   // under g_lock
std::atomic<uint64_t> c_served{0}, c_commits{0}, c_fallback{0}, c_commitFail{0}, c_doubleFree{0}, c_decommitKB{0};

bool Owned(const void* p) {
    const uint8_t* b = g_base.load(std::memory_order_acquire);
    return b && p >= b && p < b + uint64_t(kSlices) * kSlice;
}

// The game's allocator (ICoreAllocator: vtable +8 Alloc(size, name, flags), +0xC Free(ptr, size); thiscall)
void* GameAlloc(void* a, DWORD size, const char* name) {
    return reinterpret_cast<void*(__thiscall*)(void*, DWORD, const char*, DWORD)>((*reinterpret_cast<void***>(a))[2])(a, size, name, 0);
}
void GameFree(void* a, void* p) { reinterpret_cast<void(__thiscall*)(void*, void*, DWORD)>((*reinterpret_cast<void***>(a))[3])(a, p, 0); }

// Slices [s, s + n) back to free; lock held
void FreeRun(uint32_t s, uint32_t n, DWORD now) {
    for (uint32_t j = s; j < s + n; j++) g_state[j] = kFree, g_freedAt[j] = now;
}

void* Take(DWORD size) {
    const uint32_t n = (size + kSlice - 1) / kSlice;
    bool dirty[kMaxBytes / kSlice];
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
        commit |= !dirty[j];
        g_committed[best + j] = true;
    }
    g_head[best] = static_cast<uint16_t>(n);
    g_live++;
    ReleaseSRWLockExclusive(&g_lock);
    uint8_t* const p = base + best * kSlice;
    if (commit) {
        c_commits.fetch_add(1, std::memory_order_relaxed);
        if (!VirtualAlloc(p, n * kSlice, MEM_COMMIT, PAGE_READWRITE)) { // commit limit: the game's call
            AcquireSRWLockExclusive(&g_lock);
            for (uint32_t j = 0; j < n; j++) g_committed[best + j] = dirty[j];
            g_head[best] = 0;
            g_live--;
            FreeRun(best, n, GetTickCount());
            ReleaseSRWLockExclusive(&g_lock);
            c_commitFail.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    // A fresh commit is zero pages; a reused slice holds an older block's bytes. The loader accepts a short read, so the
    // tail of a buffer can be read unwritten: zeros, as from the game's own fresh block
    for (uint32_t j = 0; j < n; j++)
        if (dirty[j]) std::memset(p + j * kSlice, 0, (std::min<uint32_t>)(kSlice, size - j * kSlice));
    c_served.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void Put(uint8_t* p) {
    const uint32_t s = static_cast<uint32_t>(p - g_base.load(std::memory_order_relaxed)) / kSlice;
    AcquireSRWLockExclusive(&g_lock);
    if ((reinterpret_cast<uintptr_t>(p) & (kSlice - 1)) || g_state[s] != kUsed || !g_head[s]) {
        ReleaseSRWLockExclusive(&g_lock);
        c_doubleFree.fetch_add(1, std::memory_order_relaxed); // never handed to the game's free
        return;
    }
    FreeRun(s, g_head[s], GetTickCount());
    g_head[s] = 0;
    g_live--;
    ReleaseSRWLockExclusive(&g_lock);
}

// In place of the allocator call in the SharedPointer constructor (reached only from MemoryStream::Realloc)
void* __stdcall StubAlloc(DWORD size, void* /*stream*/, void* allocator, const char* name) {
    if (size >= kMinBytes && size <= kMaxBytes && g_armed.load(std::memory_order_relaxed))
        if (void* p = Take(size)) return p;
    return GameAlloc(allocator, size, name);
}

// In place of allocator->Free(data, 0); sp = the SharedPointer (+4 allocator, +8 data)
void __stdcall StubFree(void* sp) {
    auto* const f = static_cast<uint8_t* const*>(sp);
    void* const allocator = reinterpret_cast<void* const*>(sp)[1];
    uint8_t* const p = f[2];
    if (Owned(p)) Put(p);
    else GameFree(allocator, p);
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
        for (uint32_t j = runs[r][0]; j < runs[r][0] + runs[r][1]; j++) g_state[j] = kFree, g_committed[j] = !ok;
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
    for (;;) {
        Sleep(kUpkeepMs);
        Decommit(kIdleMs);
        if (ReleaseIfRetired()) return 0;
    }
}

bool InstallSites() {
    const struct {
        uintptr_t fn;
        const BYTE* bytes;
        size_t n;
    } windows[] = {{kCtor, kCtorBytes, sizeof kCtorBytes}, {kRelease, kReleaseBytes, sizeof kReleaseBytes}, {kRealloc, kReallocBytes, sizeof kReallocBytes}};
    for (const auto& w : windows)
        if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(w.fn), w.bytes, w.n)) {
            LOG_INFO(std::format("[StreamArena] The stream code at {:#010x} differs from Steam 1.67.2: off", w.fn));
            return false;
        }
    static BYTE freeRel[16], freeRealloc[16], alloc[12];
    const auto freeSite = [](BYTE* b, uintptr_t at) { // push esi; call StubFree; jmp +8; int3 x 8
        const int32_t r = MemPatch::CalculateRelativeOffset(at + 1, reinterpret_cast<uintptr_t>(&StubFree), 5);
        b[0] = 0x56, b[1] = 0xE8, std::memcpy(b + 2, &r, 4), b[6] = 0xEB, b[7] = 0x08, std::memset(b + 8, 0xCC, 8);
    };
    freeSite(freeRel, kReleaseAt);
    freeSite(freeRealloc, kReallocAt);
    // push ecx (name); push eax (allocator); push ebx (the stream: Realloc's this); push [esp+14h] (size); call StubAlloc.
    // Four arguments, stdcall: the stack is as after the original three-argument thiscall, and the return stays 0x004DEC17
    constexpr BYTE head[7] = {0x51, 0x50, 0x53, 0xFF, 0x74, 0x24, 0x14};
    std::memcpy(alloc, head, 7);
    alloc[7] = 0xE8;
    const int32_t r = MemPatch::CalculateRelativeOffset(kCtorAt + 7, reinterpret_cast<uintptr_t>(&StubAlloc), 5);
    std::memcpy(alloc + 8, &r, 4);
    const MemPatch::CodeWrite writes[] = {{kReleaseAt, freeRel, 16, 16}, {kReallocAt, freeRealloc, 16, 16}, {kCtorAt, alloc, 12, 12}};
    if (MemPatch::WriteCodeBatchSuspended(writes, 3)) return true;
    // a write failed: the earlier ones stay and pass through (nothing is armed, so no arena block exists)
    LOG_WARNING("[StreamArena] A stream site could not be written: off");
    return false;
}

} // namespace

void Install() {
    if (!DevSwitches::StreamArena()) return;
    uint8_t* const base = static_cast<uint8_t*>(VirtualAlloc(nullptr, uint64_t(kSlices) * kSlice, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE));
    if (!base) {
        LOG_INFO("[StreamArena] No room for the 48 MB arena: off");
        return;
    }
    if (!InstallSites()) {
        VirtualFree(base, 0, MEM_RELEASE);
        return;
    }
    g_base.store(base, std::memory_order_release);
    g_armed.store(true, std::memory_order_release);
    if (HANDLE t = CreateThread(nullptr, 0, Upkeep, nullptr, 0, nullptr)) CloseHandle(t);
    LOG_INFO(std::format("[StreamArena] On: stream buffers of {} KB to {} MB from one 48 MB arena at {:#010x}", kMinBytes >> 10, kMaxBytes >> 20,
                         reinterpret_cast<uintptr_t>(base)));
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
    return std::format("{} served, {} commit calls, {} fallbacks, {} commit failures, {} double frees, {} MB decommitted; {} in use, {} MB committed",
                       c_served.load(), c_commits.load(), c_fallback.load(), c_commitFail.load(), c_doubleFree.load(), c_decommitKB.load() >> 10, live,
                       committed * (kSlice >> 10) >> 10);
}

} // namespace StreamArena

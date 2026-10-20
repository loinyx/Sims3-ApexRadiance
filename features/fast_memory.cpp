// Faster memory handling: the game allocator's lock spin and its big-block release outside the lock (see fast_memory.h and
// docs/features/performance.md).
//
// Part of Apex Radiance. Credits: @loinyx
#include "fast_memory.h"
#include "memory_telemetry.h"
#include <intrin.h>
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace FastMemory {
namespace {

constexpr uint32_t kCsPointer = 0x4E4;     // allocator: pointer to its CRITICAL_SECTION
constexpr uint32_t kCsEmbedded = 0x4E8;    // ... which is embedded here
constexpr DWORD kSpin = 2000;              // Windows' default for InitializeCriticalSection on a multi-core PC
constexpr int kAllocSites = 5;             // the allocator's VirtualAlloc calls (0x004E4E81, 4EB4, 4EDB, 512A, 5536 on Steam 1.67.2)
constexpr intptr_t kScanBefore = 0x600;    // they lie in [release call - 0x600, release call + 0x300) (Steam: -0x485 .. +0x230)
constexpr intptr_t kScanAfter = 0x300;
constexpr uint32_t kQueueCap = 32;
constexpr int kCallLen = 6;                // call dword ptr [imm32]

std::mutex g_ctrl;
bool g_started = false;
std::atomic<bool> g_on{false};

CRITICAL_SECTION* g_cs = nullptr;
uint32_t g_spinBefore = 0;
bool g_spinSet = false;

uintptr_t g_allocSlot = 0, g_freeSlot = 0; // the game's import slots (called through, so another module's IAT hook is kept)
struct CallSite {
    uintptr_t at = 0;
    uint8_t orig[kCallLen] = {};
    bool written = false;
};
CallSite g_freeSite;
CallSite g_allocSites[kAllocSites];

// ---- the release queue ----
SRWLOCK g_qLock = SRWLOCK_INIT;
void* g_queue[kQueueCap];
uint32_t g_queued = 0;       // under g_qLock
bool g_workerAlive = false;  // under g_qLock: false = release on the calling thread
HANDLE g_wake = nullptr;
HANDLE g_worker = nullptr;
std::atomic<bool> g_workerStop{false};

std::atomic<uint64_t> c_deferred{0}, c_direct{0}, c_retries{0}, c_retryFailed{0}, g_releaseTicks{0};
std::atomic<uint32_t> g_maxQueued{0};
double g_qpcMs = 0.0;

using FreeFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
using AllocFn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
inline FreeFn RealFree() { return *reinterpret_cast<FreeFn*>(g_freeSlot); }
inline AllocFn RealAlloc() { return *reinterpret_cast<AllocFn*>(g_allocSlot); }

uint64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// Every drain holds this from taking the batch until its last VirtualFree returned, and bumps g_releasedGen after it: a
// failing VirtualAlloc waits for a drain in progress (its blocks may be what the call needs) and knows whether one ended
SRWLOCK g_drainLock = SRWLOCK_INIT;
std::atomic<uint32_t> g_releasedGen{0};
HANDLE g_workerDone = nullptr; // set by the worker when it leaves its loop (a thread's exit waits for the loader lock)

// Releases every queued block on the calling thread. Returns how many.
uint32_t ReleaseQueued() {
    void* batch[kQueueCap];
    AcquireSRWLockExclusive(&g_drainLock);
    AcquireSRWLockExclusive(&g_qLock);
    const uint32_t n = g_queued;
    std::memcpy(batch, g_queue, n * sizeof(void*));
    g_queued = 0;
    ReleaseSRWLockExclusive(&g_qLock);
    if (n) {
        const uint64_t t0 = Qpc();
        const FreeFn f = RealFree();
        for (uint32_t i = 0; i < n; i++) f(batch[i], 0, MEM_RELEASE);
        g_releaseTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
        g_releasedGen.fetch_add(1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_drainLock);
    return n;
}

DWORD WINAPI WorkerProc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // the queue is short-lived address space: empty it promptly
    while (!g_workerStop.load(std::memory_order_acquire)) {
        WaitForSingleObject(g_wake, 100);
        c_deferred.fetch_add(ReleaseQueued(), std::memory_order_relaxed);
    }
    SetEvent(g_workerDone);
    return 0;
}

// Replaces "call [VirtualFree]" in FreeInternal: VirtualFree(base, 0, MEM_RELEASE) of a big block, allocator lock held.
// The allocator ignores the result. TryAcquire: at process exit the worker may have been ended inside the queue lock.
BOOL WINAPI Stub_Free(LPVOID address, SIZE_T size, DWORD type) {
    if (type == MEM_RELEASE && size == 0 && address && TryAcquireSRWLockExclusive(&g_qLock)) {
        if (g_workerAlive && !(kMemoryOptimizationsEnabled && MemoryTelemetry::saveDepth.load(std::memory_order_acquire)) && g_queued < kQueueCap) {
            g_queue[g_queued++] = address;
            const uint32_t q = g_queued;
            ReleaseSRWLockExclusive(&g_qLock);
            uint32_t m = g_maxQueued.load(std::memory_order_relaxed);
            while (q > m && !g_maxQueued.compare_exchange_weak(m, q, std::memory_order_relaxed)) {}
            SetEvent(g_wake);
            return TRUE;
        }
        ReleaseSRWLockExclusive(&g_qLock);
    }
    c_direct.fetch_add(1, std::memory_order_relaxed);
    return RealFree()(address, size, type);
}

// Replaces the allocator's "call [VirtualAlloc]": the same call; when it fails while releases are queued, they are
// released here and the call is made once more
LPVOID WINAPI Stub_Alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    const AllocFn alloc = RealAlloc();
    const uint32_t gen = g_releasedGen.load(std::memory_order_acquire);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto observed = [&]() {
        LPVOID result = alloc(address, size, type, protect);
        const DWORD lastError = GetLastError();
        MemoryTelemetry::Attempt(size, result != nullptr, caller, result ? 0 : lastError, type);
        SetLastError(lastError);
        return result;
    };
    LPVOID r = observed();
    if (r) return r;
    const DWORD err = GetLastError();
    // ReleaseQueued waits for a drain in progress; a drain that ended since before the call also counts
    if (!ReleaseQueued() && g_releasedGen.load(std::memory_order_acquire) == gen) {
        SetLastError(err);
        return r;
    }
    c_retries.fetch_add(1, std::memory_order_relaxed);
    r = observed();
    if (!r) c_retryFailed.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// ---- helpers (no C++ objects in the SEH functions) ----
bool ReadU32(uintptr_t a, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The game's import slot of kernel32!name (the address its "call [slot]" instructions read), or 0
uintptr_t ImportSlotRaw(const char* name) {
    __try {
        const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return 0;
        for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "kernel32.dll") != 0 || !d->OriginalFirstThunk) continue;
            auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + d->OriginalFirstThunk);
            auto slots = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; names++, slots++) {
                if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
                const auto ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) == 0) return reinterpret_cast<uintptr_t>(&slots->u1.Function);
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// True when the import slot currently holds kernel32!name (or the KernelBase function it forwards to)
bool SlotHolds(uintptr_t slot, const char* name) {
    uint32_t v = 0;
    if (!slot || !MemPatch::ReadBytes(slot, reinterpret_cast<uint8_t*>(&v), 4) || !v) return false;
    for (const wchar_t* dll : {L"kernel32.dll", L"kernelbase.dll"}) {
        const HMODULE m = GetModuleHandleW(dll);
        if (m && reinterpret_cast<uintptr_t>(GetProcAddress(m, name)) == v) return true;
    }
    return false;
}

// The slot read by the "call [slot]" at `at`, when that slot holds kernel32!name; 0 otherwise. Used when the game's own
// import directory does not list kernel32: the EA app build's import directory belongs to its activation stub, which
// fills the real import slots itself at start-up.
uintptr_t SlotFromCall(uintptr_t at, const char* name) {
    uint8_t b[6];
    if (!at || !MemPatch::ReadBytes(at, b, 6) || b[0] != 0xFF || b[1] != 0x15) return 0;
    uint32_t s;
    std::memcpy(&s, b + 2, 4);
    return SlotHolds(s, name) ? s : 0;
}

bool IsCallThrough(uintptr_t at, uintptr_t slot) {
    uint8_t b[kCallLen];
    if (!MemPatch::ReadBytes(at, b, kCallLen) || b[0] != 0xFF || b[1] != 0x15) return false;
    uint32_t s;
    std::memcpy(&s, b + 2, 4);
    return s == slot;
}

// "call [slot]" (6 bytes) -> "nop; call stub": the CALL ends where the original did, so a thread still inside the stub when
// the original bytes are written back returns to the same instruction boundary
bool Redirect(CallSite& site, uintptr_t at, void* stub) {
    site.at = at;
    if (!MemPatch::ReadBytes(at, site.orig, kCallLen)) return false;
    uint8_t b[kCallLen] = {0x90, 0xE8, 0, 0, 0, 0};
    const int32_t rel = MemPatch::CalculateRelativeOffset(at + 1, reinterpret_cast<uintptr_t>(stub), 5);
    std::memcpy(b + 2, &rel, 4);
    site.written = MemPatch::WriteCodeSuspended(at, b, kCallLen);
    return site.written;
}

bool Restore(CallSite& site) {
    if (!site.written) return true;
    if (!MemPatch::WriteCodeSuspended(site.at, site.orig, kCallLen)) return false;
    site.written = false;
    return true;
}

// Deferral on: the allocator's VirtualAlloc calls first (the retry must exist before anything is queued), then the release
bool StartDeferral(std::string* why) {
    const uintptr_t freeAt = GameAddr::Get(GameAddr::Id::AllocMmapFreeCall);
    if (!freeAt) {
        *why = "the allocator's big-block release was not found";
        return false;
    }
    g_allocSlot = ImportSlotRaw("VirtualAlloc");
    g_freeSlot = ImportSlotRaw("VirtualFree");
    if (!g_freeSlot) g_freeSlot = SlotFromCall(freeAt, "VirtualFree");
    if (!g_allocSlot) {
        // The first call in the allocator that reads a slot holding VirtualAlloc
        for (uintptr_t a = freeAt - kScanBefore; a < freeAt + kScanAfter && !g_allocSlot; a++) g_allocSlot = SlotFromCall(a, "VirtualAlloc");
    }
    if (!g_allocSlot || !g_freeSlot) {
        *why = "the game's VirtualAlloc / VirtualFree imports were not found";
        return false;
    }
    if (!IsCallThrough(freeAt, g_freeSlot)) {
        *why = std::format("the big-block release {:#010x} is not the expected call [VirtualFree]", freeAt);
        return false;
    }
    uintptr_t found[kAllocSites];
    int n = 0;
    for (uintptr_t a = freeAt - kScanBefore; a < freeAt + kScanAfter; a++) {
        if (!IsCallThrough(a, g_allocSlot)) continue;
        if (n < kAllocSites) found[n] = a;
        n++;
    }
    if (n != kAllocSites) {
        *why = std::format("{} VirtualAlloc calls found in the allocator (expected {})", n, kAllocSites);
        return false;
    }
    // Both events live for the process (never closed: a stub may still hold the handle while Stop runs)
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_workerDone) g_workerDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_wake || !g_workerDone) {
        *why = "the helper thread's events could not be created";
        return false;
    }
    ResetEvent(g_workerDone);
    g_workerStop.store(false);
    g_worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (!g_worker) {
        *why = "the helper thread could not be started";
        return false;
    }
    AcquireSRWLockExclusive(&g_qLock);
    g_workerAlive = true;
    ReleaseSRWLockExclusive(&g_qLock);
    bool ok = true;
    for (int i = 0; i < kAllocSites && ok; i++) ok = Redirect(g_allocSites[i], found[i], reinterpret_cast<void*>(&Stub_Alloc));
    if (ok) ok = Redirect(g_freeSite, freeAt, reinterpret_cast<void*>(&Stub_Free));
    if (ok) return true;
    *why = "a call could not be rewritten (a thread was stopped on it)";
    return false; // the caller runs StopDeferral
}

// The release first (nothing new is queued), then the queue emptied here, then the VirtualAlloc calls. False when a call
// could not be written back (it stays redirected: the stubs keep working, releasing on the calling thread).
bool StopDeferral() {
    bool ok = Restore(g_freeSite);
    AcquireSRWLockExclusive(&g_qLock);
    g_workerAlive = false;
    ReleaseSRWLockExclusive(&g_qLock);
    if (g_worker) {
        // Wait for the worker to leave its loop, not for the thread to end: on FreeLibrary the loader lock is held here
        // and a thread's exit needs it
        g_workerStop.store(true, std::memory_order_release);
        SetEvent(g_wake);
        WaitForSingleObject(g_workerDone, 2000);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    c_deferred.fetch_add(ReleaseQueued(), std::memory_order_relaxed);
    for (auto& s : g_allocSites) ok = Restore(s) && ok;
    return ok;
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("FastMemory", &missing)) return fail(GameAddr::NotAvailable(missing));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    uint32_t alloc = 0, csPtr = 0;
    if (!ReadU32(GameAddr::Get(GameAddr::Id::AllocGlobal), &alloc) || !alloc || !ReadU32(alloc + kCsPointer, &csPtr) || csPtr != alloc + kCsEmbedded)
        return fail(std::format("The game's allocator was not recognised (allocator {:#010x}, lock {:#010x})", alloc, csPtr));
    g_cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<uintptr_t>(csPtr));
    std::string why;
    if (!StartDeferral(&why)) {
        StopDeferral();
        return fail("Big-block release: " + why);
    }
    // Only the low 24 bits (the spin) change; Windows keeps the flag bits
    g_spinBefore = static_cast<uint32_t>(SetCriticalSectionSpinCount(g_cs, kSpin)) & 0x00FFFFFFu;
    g_spinSet = true;
    g_on.store(true, std::memory_order_release);
    MemoryTelemetry::active.store(true, std::memory_order_relaxed);
    g_started = true;
    LOG_INFO(std::format("[FastMemory] On: allocator {:#010x}, lock spin {} -> {}; big-block release {:#010x} queued to a helper thread; {} VirtualAlloc calls "
                         "retry after releasing the queue",
                         alloc, g_spinBefore, kSpin, g_freeSite.at, kAllocSites));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release);
    MemoryTelemetry::active.store(false, std::memory_order_relaxed);
    if (g_spinSet) SetCriticalSectionSpinCount(g_cs, g_spinBefore);
    g_spinSet = false;
    const bool restored = StopDeferral();
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[FastMemory] Off (released by the helper {}, on the calling thread {}, most queued {}, VirtualAlloc retries {} ({} failed)){}", s.deferred,
                         s.direct, s.maxQueued, s.allocRetries, s.allocRetryFailed,
                         restored ? "" : "; a call could not be written back (it stays redirected and releases on the calling thread)"));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

uint32_t PrepareForSave() {
    if (!Running()) return 0;
    const auto released = ReleaseQueued();
    c_direct.fetch_add(released, std::memory_order_relaxed);
    return released;
}

Stats GetStats() {
    Stats s;
    if (g_cs) {
        uint32_t v = 0;
        if (ReadU32(reinterpret_cast<uintptr_t>(g_cs) + offsetof(CRITICAL_SECTION, SpinCount), &v)) s.spinNow = v & 0x00FFFFFFu;
        s.spinBefore = g_spinBefore;
    }
    s.deferring = g_freeSite.written;
    s.deferred = c_deferred.load();
    s.direct = c_direct.load();
    s.maxQueued = g_maxQueued.load();
    s.allocRetries = c_retries.load();
    s.allocRetryFailed = c_retryFailed.load();
    s.releaseMs = static_cast<double>(g_releaseTicks.load()) * g_qpcMs;
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    return std::format("On: lock spin {}; {} big blocks released by the helper thread outside the lock ({:.1f} ms of release work in all)", s.spinNow, s.deferred,
                       s.releaseMs);
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    ImGui::TextUnformatted(("Faster memory handling: " + StatusText()).c_str());
    ImGui::TextDisabled("Spin %u (was %u); released by the helper %llu, on the calling thread %llu, most queued %u; VirtualAlloc retries %llu (failed %llu)", s.spinNow,
                        s.spinBefore, static_cast<unsigned long long>(s.deferred), static_cast<unsigned long long>(s.direct), s.maxQueued,
                        static_cast<unsigned long long>(s.allocRetries), static_cast<unsigned long long>(s.allocRetryFailed));
}

} // namespace FastMemory

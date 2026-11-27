// Full lot budget during lot waits (see lot_wait.h).
//
// Addresses (Steam 1.67.2 / EA app 1.69.47, found by signature, one match each in both builds):
//  - IC World.WaitForDetailedLots (0x0078F1A0 / 0x00790330), cdecl(delegate, lot lo, lot hi, flags), result in edx:eax.
//    Makes a waiter (+0x18/+0x1C the lot, +0x20 the flags), registers it for the lot message and sleeps the task.
//  - Done (0x0078E8F0 / 0x0078FA80), thiscall(waiter, result byte), ret 4: wakes the task; called by the lot check when
//    the lot is ready or there is no world.
#include "lot_wait.h"
#include "lot_wait_logic.h"
#include "perf_toggles.h"
#include "apex_log.h"
#include "hook_chain.h"
#include "memory_patch.h"
#include <windows.h>
#include <atomic>
#include <format>

namespace {

constexpr const char* kSigStart = "83 EC 20 53 55 56 57 E8 ?? ?? ?? ?? 8B F0 8B 06 8B 90 18 01 00 00 8B CE FF D2 6A 00 6A 00 6A 00 6A 00 68 ?? ?? ?? ?? 6A 28 8B E8 8B DA E8 ?? ?? ?? ?? 83 C4 18 85 C0 74 7C 8B 4C 24 40";
constexpr const char* kSigDone = "83 EC 20 56 8B F1 E8 ?? ?? ?? ?? 85 C0 74 51 8A 4C 24 28 8B 10 8B 92 24 01 00 00 88 4C 24 14 8D 4C 24 04 51 8B 4E 14 51 8B 4E 10 51";

using StartFn = uint64_t(__cdecl*)(void* deleg, uint32_t lo, uint32_t hi, uint32_t flags);
using DoneFn = void(__thiscall*)(void* waiter, uint32_t result);
StartFn g_start = nullptr;
DoneFn g_done = nullptr;

SRWLOCK g_lock = SRWLOCK_INIT; // the table: the hooks (script thread) and KeepFullBudget's expiry
LotWaitLogic::Table g_table;
std::atomic<int> g_open{0};          // the table's count, read without the lock
std::atomic<uint64_t> g_oldest{~0ull}; // the oldest open wait's start (GetTickCount64)

void Mirror() { // under the lock
    g_open.store(g_table.Count(), std::memory_order_relaxed);
    g_oldest.store(g_table.OldestStart(), std::memory_order_relaxed);
}

bool ReadWaiter(const void* w, uint64_t& lot, uint32_t& flags) {
    __try {
        const auto* p = static_cast<const uint8_t*>(w);
        lot = *reinterpret_cast<const uint32_t*>(p + 0x18) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(p + 0x1C)) << 32;
        flags = *reinterpret_cast<const uint32_t*>(p + 0x20);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint64_t __cdecl HookStart(void* deleg, uint32_t lo, uint32_t hi, uint32_t flags) {
    const uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    g_table.Expire(now);
    g_table.Open(lo | static_cast<uint64_t>(hi) << 32, flags, now);
    Mirror();
    ReleaseSRWLockExclusive(&g_lock);
    return g_start(deleg, lo, hi, flags); // sleeps the task until its done
}

void __fastcall HookDone(void* waiter, void*, uint32_t result) {
    uint64_t lot = 0;
    uint32_t flags = 0;
    if (ReadWaiter(waiter, lot, flags)) {
        AcquireSRWLockExclusive(&g_lock);
        g_table.Close(lot, flags);
        Mirror();
        ReleaseSRWLockExclusive(&g_lock);
    }
    g_done(waiter, result);
}

uintptr_t Unique(BYTE* base, size_t size, const char* sig) {
    const uintptr_t hit = MemPatch::ScanPattern(base, size, sig);
    if (!hit) return 0;
    const uintptr_t next = hit + 1, end = reinterpret_cast<uintptr_t>(base) + size;
    return MemPatch::ScanPattern(reinterpret_cast<BYTE*>(next), end - next, sig) ? 0 : hit;
}

} // namespace

namespace LotWait {

void Install() {
    BYTE* base = nullptr;
    size_t size = 0;
    if (!MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size)) return;
    const uintptr_t start = Unique(base, size, kSigStart), done = Unique(base, size, kSigDone);
    if (!start || !done) {
        LOG_WARNING(std::format("[LotWait] Off: the lot wait's {} not found once in this game build (the camera-motion cuts run during lot waits)",
                                !start ? "start" : "done"));
        return;
    }
    g_start = reinterpret_cast<StartFn>(start);
    g_done = reinterpret_cast<DoneFn>(done);
    if (!DetourBatch::InstallHooks({{reinterpret_cast<void**>(&g_start), reinterpret_cast<void*>(&HookStart)},
                                    {reinterpret_cast<void**>(&g_done), reinterpret_cast<void*>(&HookDone)}})) {
        LOG_WARNING("[LotWait] Off: the lot wait could not be hooked (the camera-motion cuts run during lot waits)");
        return;
    }
    LOG_INFO(std::format("[LotWait] On: WaitForDetailedLots {}, done {}; while a lot wait is open (at most {} s) lot lighting and wall shading keep the game's budget",
                         HookChain::AddressText(reinterpret_cast<const void*>(start)), HookChain::AddressText(reinterpret_cast<const void*>(done)),
                         LotWaitLogic::kCeilingMs / 1000));
}

bool KeepFullBudget() {
    if (!PerfToggles::LotWaitFullBudget.load(std::memory_order_relaxed) || g_open.load(std::memory_order_relaxed) <= 0) return false;
    const uint64_t now = GetTickCount64();
    if (now - g_oldest.load(std::memory_order_relaxed) <= LotWaitLogic::kCeilingMs) return true;
    AcquireSRWLockExclusive(&g_lock); // a wait passed the ceiling: free it
    g_table.Expire(now);
    Mirror();
    const bool open = g_table.Count() > 0;
    ReleaseSRWLockExclusive(&g_lock);
    return open;
}

} // namespace LotWait

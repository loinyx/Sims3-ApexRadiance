#include "thumbnail_progress.h"
#if defined(APEX_LOADING_DIAGNOSTICS)
#include "thumbnail_progress_state.h"
#include "memory_patch.h"
#include "apex_log.h"
#include <atomic>
#include <format>
#include <intrin.h>

namespace ThumbnailProgress {
namespace {
using Update = void(__fastcall*)(BYTE*, void*);
using Ready = bool(__fastcall*)(BYTE*, void*);
using Lighting = void(__thiscall*)(BYTE*, bool);
using LotLighting = void(__thiscall*)(BYTE*);
Update update = nullptr;
Ready ready = nullptr;
Lighting lighting = nullptr;
LotLighting lotLighting = nullptr;
std::atomic<DWORD> ownerThread{0};
std::atomic<BYTE*> ownerRoot{nullptr};
std::atomic<uint64_t> nativeTurn{0};
std::atomic_flag recovering = ATOMIC_FLAG_INIT;
Waits waits; // thumbnail service thread only
unsigned reports = 0;

bool RootState(BYTE* root, bool* busy, bool* paused, BYTE** service) noexcept {
    __try {
        if (!root || root != *reinterpret_cast<BYTE**>(0x011D1860)) return false;
        const auto control = *reinterpret_cast<const BYTE* const*>(0x011CECC8);
        if (!control) return false;
        *busy = root[0x290] != 0;
        *paused = control[0x8D] != 0;
        *service = *reinterpret_cast<BYTE**>(root + 0x1C0);
        return *service != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool LotState(BYTE* renderer, uint64_t* lot, BYTE** manager, bool* enabled, bool* suspended) noexcept {
    __try {
        *lot = *reinterpret_cast<const uint64_t*>(renderer + 0x10);
        *manager = *reinterpret_cast<BYTE**>(renderer + 0x23C);
        if (!*manager || !renderer[0x1E] || renderer[0x1F]) return false;
        *enabled = (*manager)[0x18] != 0;
        *suspended = (*manager)[0x4E] != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __fastcall UpdateHook(BYTE* root, void* edx) {
    bool busy = true, paused = true;
    BYTE* service = nullptr;
    if (RootState(root, &busy, &paused, &service) && !busy) {
        ownerRoot.store(root, std::memory_order_relaxed);
        ownerThread.store(GetCurrentThreadId(), std::memory_order_release);
        if (!paused) nativeTurn.store(GetTickCount64(), std::memory_order_relaxed);
    }
    update(root, edx);
}
bool Check(BYTE* renderer, void* edx, const void* caller) {
    const bool result = ready(renderer, edx);
    // Only ThumbnailManager's state 23, never other callers of the shared predicate.
    if (caller != reinterpret_cast<void*>(0x00D5BE34)) return result;
    const auto thread = GetCurrentThreadId();
    if (recovering.test_and_set(std::memory_order_acquire)) return result;
    struct Release { ~Release() { recovering.clear(std::memory_order_release); } } release;
    uint64_t lot = 0;
    BYTE* manager = nullptr;
    bool enabled = false, suspended = true;
    if (!LotState(renderer, &lot, &manager, &enabled, &suspended)) return result;
    const auto now = GetTickCount64();
    Wait* wait = waits.Observe(reinterpret_cast<uintptr_t>(renderer), lot, now, result);
    if (!wait) return result;
    BYTE* service = nullptr;
    bool busy = true, paused = true;
    const auto owner = ownerThread.load(std::memory_order_acquire);
    const auto root = ownerRoot.load(std::memory_order_relaxed);
    const bool readable = RootState(root, &busy, &paused, &service);
    const auto lastNative = nativeTurn.load(std::memory_order_relaxed);
    if (!readable || !Due(*wait, now, owner == thread, busy, enabled, suspended, lastNative)) {
        if (!wait->reported && now - wait->since >= 1500 && reports < 32) {
            LOG_INFO(std::format("[ThumbnailProgress] Lot {:016X}: pending; thread {} owner {}; native age {} ms; readable {}; scene busy {}; enabled {}; suspended {}.",
                lot, thread, owner, lastNative ? now - lastNative : 0, readable, busy, enabled, suspended));
            ++reports;
            wait->reported = true;
        }
        return result;
    }
    wait->pumped = now;
    ++wait->turns;
    // The native lot-job wait also calls Lighting::Update(false), at 0x00AD984B.
    // It gathers dirty rooms and picks work without advancing animation or physics.
    // The native lot pass then uses its own stopwatch, budget and complete solves.
    // Runtime effectiveness of this recovery remains unverified until a game test.
    lighting(service, false);
    lotLighting(manager);
    const bool completed = ready(renderer, edx);
    if (reports < 32 && (wait->turns == 1 || completed)) {
        LOG_INFO(std::format("[ThumbnailProgress] Lot {:016X}: native lighting recovery turn {}; ready {}; paused {}; waited {} ms.",
            lot, wait->turns, completed, paused, now - wait->since));
        ++reports;
    }
    if (completed) waits.Observe(reinterpret_cast<uintptr_t>(renderer), lot, now, true);
    return completed;
}
bool __fastcall ReadyHook(BYTE* renderer, void* edx) {
    return Check(renderer, edx, _ReturnAddress());
}
}
void Start() {
    if (g_exeTimestamp != 0x52DEC247) return;
    const GameAddress worldEntry{"Thumbnail progress owner", {},
        "51 56 8B F1 80 BE 90 02 00 00 00 0F 85 EA 01 00 00 A1 C8 EC 1C 01 80 B8 8D 00 00 00 00", 0, {0x51,0x56,0x8B,0xF1}};
    const GameAddress readyEntry{"Thumbnail progress readiness", {},
        "8B 89 3C 02 00 00 85 C9 74 05 E9 71 B5 FF FF 32 C0 C3", 0, {0x8B,0x89,0x3C,0x02,0,0}};
    const GameAddress lightingEntry{"Thumbnail progress lighting", {},
        "80 7C 24 04 00 56 8B F1 74 23 80 BE 00 01 00 00 00 74 05", 0, {0x80,0x7C,0x24,0x04,0}};
    const GameAddress lotEntry{"Thumbnail progress lot pass", {},
        "83 EC 1C 56 8B F1 80 7E 18 00 0F 84 D5 00 00 00 F3 0F 10 05 AC DD 1C 01", 0, {0x83,0xEC,0x1C,0x56}};
    const GameAddress callSite{"Thumbnail progress call site", {},
        "8B 8E 40 02 00 00 E8 7C 48 D8 FF 84 C0 0F 84 A7 E5 FF FF", 0, {0x8B,0x8E,0x40,0x02}};
    const auto w = worldEntry.Resolve(), r = readyEntry.Resolve(), l = lightingEntry.Resolve(), p = lotEntry.Resolve(), c = callSite.Resolve();
    if (!w || *w != 0x006EBB70 || !r || *r != 0x00AE06B0 || !l || *l != 0x006B0500 ||
        !p || *p != 0x00ADB8F0 || !c || *c != 0x00D5BE29) {
        LOG_WARNING("[ThumbnailProgress] Native code differs; recovery remains off.");
        return;
    }
    update = reinterpret_cast<Update>(*w);
    ready = reinterpret_cast<Ready>(*r);
    lighting = reinterpret_cast<Lighting>(*l);
    lotLighting = reinterpret_cast<LotLighting>(*p);
    const bool attached = DetourBatch::InstallHooks({
        {reinterpret_cast<void**>(&update), reinterpret_cast<void*>(&UpdateHook)},
        {reinterpret_cast<void**>(&ready), reinterpret_cast<void*>(&ReadyHook)}});
    LOG_INFO(attached ? "[ThumbnailProgress] Native lighting recovery attached; game validation pending."
                      : "[ThumbnailProgress] Hooks unavailable; recovery remains off.");
}
}
#else
namespace ThumbnailProgress { void Start() {} }
#endif

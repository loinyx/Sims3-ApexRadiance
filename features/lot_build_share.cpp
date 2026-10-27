// Shared lot build time (see lot_build_share.h). Part of Apex Radiance. Credits: @loinyx
#include "lot_build_share.h"
#include "lot_build_share_policy.h"
#include "lot_lighting_motion.h"
#include "apex_log.h"
#include "call_chain.h"
#include "dev_switches.h"
#include "game_addresses.h"
#include <windows.h>
#include <atomic>
#include <format>
#include <mutex>

namespace LotBuildShare {
namespace {

using CallChain::Layer;
using CallChain::Site;
using StagesFn = uint64_t(__fastcall*)(void* renderer, void* edx);
using SomGetterFn = void*(__cdecl*)();
using PriorityTestFn = bool(__thiscall*)(void* som, uint32_t lo, uint32_t hi);

constexpr size_t kOffLotLo = 0x10, kOffLotHi = 0x14, kOffStage = 0x24; // LotRenderer
constexpr size_t kOffWorldMode = 0x1B4;                                 // WorldManager: 0 = tool mode / loading screen
constexpr int kSlots = 32;
constexpr uint32_t kSlotStaleFrames = 120;
constexpr double kFallbackFrameMs = 50.0;

std::mutex g_ctrl;
std::atomic<bool> g_on{false};
uintptr_t g_worldGlobal = 0;
SomGetterFn g_somGetter = nullptr;
PriorityTestFn g_priorityTest = nullptr;
double g_msPerTick = 0.0;

// Render thread only (the lot pass): the frame's spending and the lots that waited
uint32_t g_frame = 0xFFFFFFFFu;
int64_t g_frameStart = 0;  // QPC; without the per-frame sampler a "frame" ends kFallbackFrameMs after it started
double g_spentMs = 0.0;
bool g_forcedThisFrame = false; // a lot that had waited kMaxSkips frames ran this frame
int g_longestWait = 0, g_longestWaitNext = 0; // the most frames a lot had waited: when this frame began / so far in it
struct Slot {
    void* renderer = nullptr;
    uint32_t frame = 0;
    int skips = 0;
};
Slot g_slots[kSlots];
thread_local uint32_t t_slice = 0; // the slice of the call in progress on this thread (0 = none)
thread_local bool t_inLotPass = false; // inside the lot pass's renderer update (CALL 0x00C7CEDF)

std::atomic<uint32_t> c_gated{0}, c_skipped{0}, c_capped{0}, c_forced{0}, c_overBudgetFrames{0};
std::atomic<uint32_t> g_maxFrameUs{0};

Slot& SlotOf(void* renderer) {
    Slot* free = nullptr;
    for (Slot& s : g_slots) {
        if (s.renderer == renderer) return s;
        if (!free && (!s.renderer || g_frame - s.frame > kSlotStaleFrames)) free = &s;
    }
    if (!free) free = &g_slots[reinterpret_cast<uintptr_t>(renderer) / 16 % kSlots];
    *free = Slot{renderer, g_frame, 0};
    return *free;
}

// The lot pass's CALL of the renderer update: marks the calls the gate may hold (the impostor builder's own updates,
// 0x00AEB3F9 / 0x00AEB41C, are not marked). A marker rather than a return address: the Frame Profiler, or another mod,
// may detour the renderer update's entry.
void __fastcall PassHook(void* renderer, void* edx, float dt) {
    const auto next = reinterpret_cast<void(__fastcall*)(void*, void*, float)>(CallChain::Next(Site::LotPassRenderer, Layer::LotBuildGate));
    t_inLotPass = true;
    next(renderer, edx, dt);
    t_inLotPass = false;
}

bool LoadingScreen() {
    const uintptr_t world = *reinterpret_cast<const uintptr_t*>(g_worldGlobal);
    return !world || *reinterpret_cast<const int*>(world + kOffWorldMode) == 0;
}

bool Priority(const uint8_t* renderer) {
    void* som = g_somGetter();
    return som && g_priorityTest(som, *reinterpret_cast<const uint32_t*>(renderer + kOffLotLo), *reinterpret_cast<const uint32_t*>(renderer + kOffLotHi));
}

void NoteFrameEnd() {
    if (g_frame == 0xFFFFFFFFu || g_spentMs <= 0.0) return;
    const uint32_t us = static_cast<uint32_t>(g_spentMs * 1000.0);
    uint32_t m = g_maxFrameUs.load(std::memory_order_relaxed);
    while (us > m && !g_maxFrameUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
    if (g_spentMs > LotBuildSharePolicy::kStillMs + 2.0) c_overBudgetFrames.fetch_add(1, std::memory_order_relaxed);
}

uint64_t __fastcall Hook(void* renderer, void* edx) {
    const auto next = reinterpret_cast<StagesFn>(CallChain::Next(Site::LotLoadStages, Layer::LotBuildGate));
    if (!g_on.load(std::memory_order_acquire) || !DevSwitches::LotBuildShare() || !renderer || !t_inLotPass || LoadingScreen())
        return next(renderer, edx);

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const uint32_t frame = LotLightingMotion::FrameIndex();
    if (frame != g_frame || static_cast<double>(t0.QuadPart - g_frameStart) * g_msPerTick > kFallbackFrameMs) {
        NoteFrameEnd();
        g_frame = frame;
        g_frameStart = t0.QuadPart;
        g_spentMs = 0.0;
        g_forcedThisFrame = false;
        g_longestWait = g_longestWaitNext;
        g_longestWaitNext = 0;
    }
    const uint8_t* r = static_cast<const uint8_t*>(renderer);
    const int stage = *reinterpret_cast<const int*>(r + kOffStage);
    Slot& slot = SlotOf(renderer);
    slot.frame = frame;
    const auto d = LotBuildSharePolicy::Decide(g_spentMs, LotLightingMotion::CameraMoving(), Priority(r), stage, slot.skips, g_forcedThisFrame, g_longestWait);
    if (stage < LotBuildSharePolicy::kFirstWaitStage) c_gated.fetch_add(1, std::memory_order_relaxed);
    if (!d.run) {
        slot.skips++;
        if (slot.skips > g_longestWaitNext) g_longestWaitNext = slot.skips;
        c_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0; // al = 0: not done; the renderer update ignores it, the stages run again next frame
    }
    if (d.forced) {
        g_forcedThisFrame = true;
        c_forced.fetch_add(1, std::memory_order_relaxed);
    }
    slot.skips = 0;

    QueryPerformanceCounter(&t0);
    t_slice = d.sliceMs;
    const uint64_t result = next(renderer, edx);
    t_slice = 0;
    QueryPerformanceCounter(&t1);
    g_spentMs += static_cast<double>(t1.QuadPart - t0.QuadPart) * g_msPerTick;
    return result;
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_on.load(std::memory_order_acquire)) return true;
    std::string missing;
    if (!GameAddr::GroupAvailable("LotBuildShare", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }
    g_worldGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    g_somGetter = reinterpret_cast<SomGetterFn>(GameAddr::Get(GameAddr::Id::PriorityLotObject));
    g_priorityTest = reinterpret_cast<PriorityTestFn>(GameAddr::Get(GameAddr::Id::PriorityLotTest));
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_msPerTick = 1000.0 / static_cast<double>(f.QuadPart);
    g_on.store(true, std::memory_order_release); // before the CALL can reach the hook
    std::string chainError;
    if (!CallChain::Install(Site::LotLoadStages, Layer::LotBuildGate, reinterpret_cast<void*>(&Hook), &chainError)) {
        g_on.store(false, std::memory_order_release);
        if (error) *error = "the lot load stages call: " + chainError;
        return false;
    }
    if (!CallChain::Install(Site::LotPassRenderer, Layer::LotBuildGate, reinterpret_cast<void*>(&PassHook), &chainError)) {
        g_on.store(false, std::memory_order_release);
        CallChain::Remove(Site::LotLoadStages, Layer::LotBuildGate);
        if (error) *error = "the lot pass call: " + chainError;
        return false;
    }
    LOG_INFO(std::format("[LotBuildShare] On: one build budget per frame for every streaming lot ({:.0f} ms, {:.0f} ms while the camera moves){}",
                         LotBuildSharePolicy::kStillMs, LotBuildSharePolicy::kMovingMs, DevSwitches::LotBuildShare() ? "" : "; development switch lot_build_share is off"));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_on.load(std::memory_order_acquire)) return;
    g_on.store(false, std::memory_order_release); // the hook passes through from now on, even if the CALL stays
    if (!CallChain::Remove(Site::LotPassRenderer, Layer::LotBuildGate) || !CallChain::Remove(Site::LotLoadStages, Layer::LotBuildGate))
        LOG_ERROR("[LotBuildShare] The lot load stages call could not be put back; the gate stays in place and passes every call through");
    LOG_INFO(std::format("[LotBuildShare] Off ({} build calls shared, {} waited a frame, {} ran after {} waits, {} slices shortened; {} frames over {:.0f} ms, longest {:.2f} ms)",
                         c_gated.load(), c_skipped.load(), c_forced.load(), LotBuildSharePolicy::kMaxSkips, c_capped.load(), c_overBudgetFrames.load(),
                         LotBuildSharePolicy::kStillMs + 2.0, g_maxFrameUs.load() / 1000.0));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

uint32_t CapSlice(uint32_t gameMs) {
    const uint32_t slice = t_slice;
    if (slice == 0 || slice >= gameMs) return gameMs;
    c_capped.fetch_add(1, std::memory_order_relaxed);
    return slice;
}

std::string StatusText() {
    if (!Running()) return "Off";
    if (!DevSwitches::LotBuildShare()) return "Off (development switch lot_build_share)";
    return std::format("On: {} build calls shared, {} waited a frame, {} slices shortened, {} frames over {:.0f} ms (longest {:.2f} ms)", c_gated.load(),
                       c_skipped.load(), c_capped.load(), c_overBudgetFrames.load(), LotBuildSharePolicy::kStillMs + 2.0, g_maxFrameUs.load() / 1000.0);
}

} // namespace LotBuildShare

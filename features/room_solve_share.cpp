// Shared room solve time while moving (see room_solve_share.h). Part of Apex Radiance. Credits: @loinyx
#include "room_solve_share.h"
#include "room_solve_share_policy.h"
#include "lot_lighting_motion.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "call_chain.h"
#include "game_addresses.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <bit>
#include <format>
#include <mutex>

namespace RoomSolveShare {
namespace {

using CallChain::Layer;
using CallChain::Site;
// 0x006A8BA0 thiscall(stopwatch*, float budget), ret 8 (the float as raw bits; x87 stack empty at the call and on return)
using SolveFn = uint64_t(__fastcall*)(void* level, void* edx, uint32_t stopwatch, uint32_t budget);
using SomGetterFn = void*(__cdecl*)();
using PriorityTestFn = bool(__thiscall*)(void* som, uint32_t lo, uint32_t hi);

constexpr size_t kOffLevelLotLo = 0x90, kOffLevelLotHi = 0x94; // lot level (story manager): its lot id
constexpr size_t kOffWorldMode = 0x1B4;                        // WorldManager: 0 = tool mode / loading screen
constexpr float kLeaveAloneMs = 100.0f;                        // budgets from here up (tool mode, synchronous solves) pass
constexpr int kSlots = 64;
constexpr uint32_t kSlotStaleFrames = 120;
constexpr double kFallbackFrameMs = 50.0;

std::mutex g_ctrl;
std::atomic<bool> g_on{false};
uintptr_t g_worldGlobal = 0;
SomGetterFn g_somGetter = nullptr;
PriorityTestFn g_priorityTest = nullptr;
double g_msPerTick = 0.0;

// Render thread only (the lot pass)
uint32_t g_frame = 0xFFFFFFFFu;
int64_t g_frameStart = 0;
double g_spentMs = 0.0;
bool g_forcedThisFrame = false;
int g_longestWait = 0, g_longestWaitNext = 0;
uint32_t g_frameRun = 0, g_frameWaited = 0;
struct Slot {
    void* level = nullptr;
    uint32_t frame = 0;
    int skips = 0;
};
Slot g_slots[kSlots];
thread_local bool t_inLotPass = false;

std::atomic<uint32_t> c_calls{0}, c_run{0}, c_waited{0}, c_forced{0}, c_waitFrames{0}, c_maxWait{0}, c_maxRunPerFrame{0}, c_maxWaitedPerFrame{0};
std::atomic<uint32_t> c_lastRun{0}, c_lastWaited{0}, g_maxFrameUs{0}, c_longestNow{0};

void Max(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t m = a.load(std::memory_order_relaxed);
    while (v > m && !a.compare_exchange_weak(m, v, std::memory_order_relaxed)) {}
}

Slot& SlotOf(void* level) {
    Slot* free = nullptr;
    for (Slot& s : g_slots) {
        if (s.level == level) return s;
        if (!free && (!s.level || g_frame - s.frame > kSlotStaleFrames)) free = &s;
    }
    if (!free) free = &g_slots[reinterpret_cast<uintptr_t>(level) / 16 % kSlots];
    *free = Slot{level, g_frame, 0};
    return *free;
}

void __fastcall PassHook(void* renderer, void* edx, float dt) {
    const auto next = reinterpret_cast<void(__fastcall*)(void*, void*, float)>(CallChain::Next(Site::LotPassRenderer, Layer::RoomSolveGate));
    t_inLotPass = true;
    next(renderer, edx, dt);
    t_inLotPass = false;
}

bool LoadingScreen() {
    const uintptr_t world = *reinterpret_cast<const uintptr_t*>(g_worldGlobal);
    return !world || *reinterpret_cast<const int*>(world + kOffWorldMode) == 0;
}

// The level's lot is the lot being played (0x006FDC80, as the game's budget 0x00ADB120 asks); unreadable = played
bool Priority(const uint8_t* level) {
    __try {
        void* som = g_somGetter();
        return !som || g_priorityTest(som, *reinterpret_cast<const uint32_t*>(level + kOffLevelLotLo), *reinterpret_cast<const uint32_t*>(level + kOffLevelLotHi));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void EndFrame() {
    if (g_frame == 0xFFFFFFFFu) return;
    if (g_frameRun || g_frameWaited) {
        c_lastRun.store(g_frameRun, std::memory_order_relaxed);
        c_lastWaited.store(g_frameWaited, std::memory_order_relaxed);
        Max(c_maxRunPerFrame, g_frameRun);
        Max(c_maxWaitedPerFrame, g_frameWaited);
        Max(g_maxFrameUs, static_cast<uint32_t>(g_spentMs * 1000.0));
        if (g_frameWaited) c_waitFrames.fetch_add(1, std::memory_order_relaxed);
    }
}

uint64_t __fastcall Hook(void* level, void* edx, uint32_t stopwatch, uint32_t budget) {
    const auto next = reinterpret_cast<SolveFn>(CallChain::Next(Site::RoomSolve, Layer::RoomSolveGate));
    if (!g_on.load(std::memory_order_acquire) || !level || !t_inLotPass || std::bit_cast<float>(budget) >= kLeaveAloneMs || LoadingScreen())
        return next(level, edx, stopwatch, budget);

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const uint32_t frame = LotLightingMotion::FrameIndex();
    if (frame != g_frame || static_cast<double>(t0.QuadPart - g_frameStart) * g_msPerTick > kFallbackFrameMs) {
        EndFrame();
        g_frame = frame;
        g_frameStart = t0.QuadPart;
        g_spentMs = 0.0;
        g_forcedThisFrame = false;
        g_longestWait = g_longestWaitNext;
        c_longestNow.store(static_cast<uint32_t>(g_longestWait), std::memory_order_relaxed);
        g_longestWaitNext = 0;
        g_frameRun = g_frameWaited = 0;
    }
    c_calls.fetch_add(1, std::memory_order_relaxed);
    const uint8_t* l = static_cast<const uint8_t*>(level);
    Slot& slot = SlotOf(level);
    slot.frame = frame;
    const auto d = RoomSolveSharePolicy::Decide(g_spentMs, LotLightingMotion::CameraMoving(), Priority(l), slot.skips, g_forcedThisFrame, g_longestWait);
    if (!d.run) {
        slot.skips++;
        if (slot.skips > g_longestWaitNext) g_longestWaitNext = slot.skips;
        if (static_cast<uint32_t>(slot.skips) > c_longestNow.load(std::memory_order_relaxed)) c_longestNow.store(slot.skips, std::memory_order_relaxed);
        g_frameWaited++;
        c_waited.fetch_add(1, std::memory_order_relaxed);
        return 0; // the call is not made: the update reads its stopwatch and goes on with its next level
    }
    if (d.forced) {
        g_forcedThisFrame = true;
        c_forced.fetch_add(1, std::memory_order_relaxed);
    }
    Max(c_maxWait, static_cast<uint32_t>(slot.skips));
    slot.skips = 0;
    g_frameRun++;
    c_run.fetch_add(1, std::memory_order_relaxed);

    QueryPerformanceCounter(&t0);
    const uint64_t result = next(level, edx, stopwatch, budget);
    QueryPerformanceCounter(&t1);
    g_spentMs += static_cast<double>(t1.QuadPart - t0.QuadPart) * g_msPerTick;
    return result;
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_on.load(std::memory_order_acquire)) return true;
    std::string missing;
    if (!GameAddr::GroupAvailable("RoomSolveShare", &missing)) {
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
    if (!CallChain::Install(Site::RoomSolve, Layer::RoomSolveGate, reinterpret_cast<void*>(&Hook), &chainError)) {
        g_on.store(false, std::memory_order_release);
        if (error) *error = "the room solve call: " + chainError;
        return false;
    }
    if (!CallChain::Install(Site::LotPassRenderer, Layer::RoomSolveGate, reinterpret_cast<void*>(&PassHook), &chainError)) {
        g_on.store(false, std::memory_order_release);
        CallChain::Remove(Site::RoomSolve, Layer::RoomSolveGate);
        if (error) *error = "the lot pass call: " + chainError;
        return false;
    }
    LOG_INFO(std::format("[RoomSolveShare] On: while the camera moves every lot's room solves share {:.0f} ms per frame (the lot being played always runs; "
                         "no level waits more than {} frames)", RoomSolveSharePolicy::kMovingMs, RoomSolveSharePolicy::kHardMaxSkips));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_on.load(std::memory_order_acquire)) return;
    g_on.store(false, std::memory_order_release); // the hook passes every call through from now on, even if the CALL stays
    if (!CallChain::Remove(Site::LotPassRenderer, Layer::RoomSolveGate) || !CallChain::Remove(Site::RoomSolve, Layer::RoomSolveGate))
        LOG_ERROR("[RoomSolveShare] The room solve call could not be put back; the gate stays in place and passes every call through");
    LOG_INFO("[RoomSolveShare] Off (" + StatusText() + ")");
}

bool Running() { return g_on.load(std::memory_order_acquire); }
uint32_t WaitedTotal() { return c_waited.load(std::memory_order_relaxed); }
uint32_t LongestWaitNow() { return Running() ? c_longestNow.load(std::memory_order_relaxed) : 0; }

std::string StatusText() {
    if (!Running()) return "Off";
    return std::format("{} solves in the lot pass, {} ran, {} waited a frame ({} after waiting {}+ frames), {} frames with a wait; longest wait {} frames; "
                       "most per frame: {} ran, {} waited; last frame {} ran, {} waited; longest frame {:.2f} ms",
                       c_calls.load(), c_run.load(), c_waited.load(), c_forced.load(), RoomSolveSharePolicy::kMaxSkips, c_waitFrames.load(), c_maxWait.load(),
                       c_maxRunPerFrame.load(), c_maxWaitedPerFrame.load(), c_lastRun.load(), c_lastWaited.load(), g_maxFrameUs.load() / 1000.0);
}

void RenderDeveloperUI() {
    if (kPublicBuild || !ImGui::GetCurrentContext()) return;
    ImGui::TextWrapped("Shared room solve time: %s", StatusText().c_str());
}

} // namespace RoomSolveShare

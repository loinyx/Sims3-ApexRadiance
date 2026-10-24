#include "transition_watch.h"
#if defined(APEX_LOADING_DIAGNOSTICS) || (defined(APEX_DEVELOPMENT_BUILD_FLAVOR) && !defined(APEX_NO_DEV_TOOLS))
#include "transition_watch_state.h"
#include "transition_watch_native.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "game_addresses.h"
#include "game_version.h"
#include "post_scene.h"
#include "room_light_queue.h"
#include "scene_budget.h"
#include "lot_lighting_motion.h"
#include "load_timing.h"
#include <Windows.h>
#include <psapi.h>
#include <array>
#include <atomic>
#include <format>

namespace TransitionWatch {
namespace {
struct Sample : Native {
    Event event = Event::None;
    uint64_t at = 0, since = 0, world = 0;
    int mode = -1, draws = -1;
    unsigned skip = 0;
    bool roomSolve = false, quickPass = false;
    SceneBudget::Stats scene{};
};
std::array<Sample, 16> queue;
SRWLOCK lock = SRWLOCK_INIT;
size_t head = 0, count = 0;
std::atomic<unsigned> lost{0};
std::atomic<uint64_t> heartbeat{0};
Tracker tracker; // render thread only

bool ReadWorld(uintptr_t* identity, int* mode, unsigned* skip) noexcept {
    __try {
        const auto global = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
        const auto world = global ? *reinterpret_cast<const uintptr_t*>(global) : 0;
        if (!world) return false;
        *identity = world;
        *mode = *reinterpret_cast<const int*>(world + 0x1B4);
        *skip = *reinterpret_cast<const unsigned char*>(world + 0x258);
        return *reinterpret_cast<const unsigned char*>(world + 0x41) != 0 && *mode >= 1 && *mode <= 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ReadNative(Sample* sample) noexcept {
    __try { ReadLots(*reinterpret_cast<const uintptr_t*>(0x011ECE58), sample); }
    __except (EXCEPTION_EXECUTE_HANDLER) { sample->readable = false; }
}
void Publish(const Sample& sample) {
    if (!TryAcquireSRWLockExclusive(&lock)) { ++lost; return; }
    if (count == queue.size()) ++lost;
    else { queue[(head + count) % queue.size()] = sample; ++count; }
    ReleaseSRWLockExclusive(&lock);
}
void Frame() {
    const uint64_t now = GetTickCount64();
    heartbeat.store(now, std::memory_order_relaxed);
    static uint64_t last = 0;
    if (now - last < 500) return;
    last = now;
    uintptr_t world = 0;
    Sample sample;
    const bool active = ReadWorld(&world, &sample.mode, &sample.skip);
    sample.draws = PostScene::DepthWritesLastFrame();
    sample.since = tracker.missingAt;
    sample.event = tracker.Update(now, world, active, PostScene::Counting(), sample.draws);
    if (sample.event == Event::None) return;
    sample.at = sample.event == Event::Resumed ? tracker.drawnAt : now;
    sample.world = world;
    if (sample.event != Event::Interrupted) sample.since = tracker.missingAt;
    ReadNative(&sample);
    sample.scene = SceneBudget::GetStats();
    sample.roomSolve = RoomLightQueue::SolveInProgress();
    sample.quickPass = RoomLightQueue::QuickPassPending();
    Publish(sample);
}
}
void Start() {
    if (g_exeTimestamp != 0x52DEC247) {
        LOG_INFO("[TransitionWatch] Native layout not verified; transition watch disabled.");
        return;
    }
    const bool ok = D3D9Hooks::RegisterPresent("TransitionWatch", [](D3D9Hooks::DeviceContext&, const RECT*, const RECT*, HWND, const RGNDATA*) {
        Frame();
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::Last);
    LOG_INFO(ok ? "[TransitionWatch] Automatic scene-gap watch enabled. Samples every 500 ms after a drawn world; reports every 10 s during a gap. Scene resumption does not prove the Edit Town dialog closed. Saving, travel and other frozen scenes can also trigger it. Frame Profiler is not required; depth-draw counting must be active."
                : "[TransitionWatch] Present observer could not attach; no transition measurements.");
}
void Pump() {
    static uint64_t last = 0, waitingSince = 0, lastStall = 0;
    static unsigned lines = 0, lastLost = 0;
    const uint64_t now = GetTickCount64();
    if (now - last < 1000 || lines >= 4096) return;
    last = now;
    std::array<Sample, 16> batch;
    size_t n = 0;
    AcquireSRWLockExclusive(&lock);
    while (count) { batch[n++] = queue[head]; head = (head + 1) % queue.size(); --count; }
    ReleaseSRWLockExclusive(&lock);
    for (size_t i = 0; i < n; ++i) {
        const auto& s = batch[i];
        const char* kind = s.event == Event::Begin ? "scene gap began" : s.event == Event::Waiting ? "scene still missing" : s.event == Event::Resumed ? "scene resumed" : "observation interrupted";
        waitingSince = s.event == Event::Begin || s.event == Event::Waiting ? s.since : 0;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        const bool memoryKnown = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
        std::string lots;
        for (unsigned j = 0; j < s.stored; ++j) {
            const auto& lot = s.lots[j];
            lots += std::format(" {:016X}:stage={}/{} done={} failed={} job={};", lot.id, lot.stage, lot.step, lot.done, lot.failed, lot.jobDone);
        }
        LOG_INFO(std::format("[TransitionWatch] {}; observed gap {:.1f} s; world {:08X} mode {} skip-streaming {}; depth draws {}; requested lots {} pending {} read {} truncated {}; scene calls {} budgeted {} forced {} left {} tracked {} stopped {}; room solve {} quick pass {}; private MiB {} known {}; lots:{}",
            kind, (s.at - s.since) / 1000.0, s.world, s.mode, s.skip, s.draws, s.requested, s.pending, s.readable, s.truncated,
            s.scene.calls, s.scene.budgeted, s.scene.fullForced, s.scene.lastLeft, s.scene.tracked, s.scene.stopped,
            s.roomSolve, s.quickPass, memory.PrivateUsage / (1024 * 1024), memoryKnown, lots));
        ++lines;
        LOG_INFO("[TransitionWatch] Cumulative work: " + LoadTiming::DiagnosticWork() + "; wall shading: " + LotLightingMotion::WallAoStatusText());
        ++lines;
    }
    const auto beat = heartbeat.load(std::memory_order_relaxed);
    if (waitingSince && beat && now - beat >= 10000 && now - lastStall >= 10000) {
        LOG_WARNING(std::format("[TransitionWatch] No Present callback for {:.1f} s during an observed scene gap. Native state is stale; the initialization pump still runs.", (now - beat) / 1000.0));
        ++lines;
        lastStall = now;
    }
    const unsigned dropped = lost.load();
    if (dropped != lastLost) {
        LOG_WARNING(std::format("[TransitionWatch] {} snapshots lost; observation is incomplete.", dropped));
        lastLost = dropped;
    }
    if (lines >= 4096) LOG_WARNING("[TransitionWatch] Session report limit reached; further reports disabled.");
}
}
#else
namespace TransitionWatch { void Start() {} void Pump() {} }
#endif

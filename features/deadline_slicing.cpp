// Frame deadline while moving (see deadline_slicing.h). Part of Apex Radiance. Credits: @loinyx
#include "deadline_slicing.h"
#include "lot_lighting_motion.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <format>
#include <mutex>
#include <vector>

namespace DeadlineSlicing {
namespace {

// Steam 1.67.2, verified in the disassembly: the JobManager service vtable's main update slot and the update's start
// (sub esp,20h; push esi; mov esi,ecx; cmp [esi+0C8h],0; je; push edi; mov edi,[esi+0CCh])
constexpr uintptr_t kJobVtableSlot = 0x00FD9810 + 0x1C;
constexpr uintptr_t kJobUpdate = 0x00599A10;
constexpr BYTE kJobUpdateBytes[] = {0x83, 0xEC, 0x20, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x74, 0x3A, 0x57, 0x8B, 0xBE, 0xCC, 0x00, 0x00, 0x00};
constexpr size_t kOffSlice = 0xCC;      // JobManager: main-thread job slice (ms, unsigned; the game's default 5)
constexpr size_t kOffWorldMode = 0x1B4; // WorldManager: 0 = tool mode / loading screen (as lot_build_share.cpp)

using UpdateFn = void(__fastcall*)(void* svc, void* edx, float a, float b);

std::mutex g_ctrl;
std::atomic<bool> g_on{false};
std::vector<MemPatch::PatchLocation> g_patches;
uintptr_t g_worldGlobal = 0;
double g_msPerTick = 0.0;

std::atomic<float> g_deadlineMs{12.0f}; // after the frame's services start
std::atomic<int> g_finalizeMs{2};       // the finalize slice while moving

// Render thread
int64_t g_frameStart = 0; // QPC at the JobManager update of this frame; 0 = unknown
bool g_moving = false;    // camera moving at this frame's start

std::atomic<uint64_t> c_frames{0}, c_movingFrames{0}, c_finalizeLowered{0}, c_sceneCut{0}, c_sceneSkip{0}, c_lotCut{0}, c_lotSkip{0};

bool LoadingScreen() {
    const uintptr_t world = g_worldGlobal ? *reinterpret_cast<const uintptr_t*>(g_worldGlobal) : 0;
    return !world || *reinterpret_cast<const int*>(world + kOffWorldMode) == 0;
}

void __fastcall JobUpdateHook(void* svc, void* edx, float a, float b) {
    const auto game = reinterpret_cast<UpdateFn>(kJobUpdate);
    if (!g_on.load(std::memory_order_acquire) || !svc) return game(svc, edx, a, b);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_frameStart = t.QuadPart;
    g_moving = LotLightingMotion::CameraMoving() && !LoadingScreen();
    c_frames.fetch_add(1, std::memory_order_relaxed);
    if (!g_moving) return game(svc, edx, a, b);
    c_movingFrames.fetch_add(1, std::memory_order_relaxed);
    auto* slice = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(svc) + kOffSlice);
    const uint32_t gameMs = *slice, ms = static_cast<uint32_t>(g_finalizeMs.load(std::memory_order_relaxed));
    if (ms >= gameMs) return game(svc, edx, a, b); // never longer than the game's (0xFFFFFFFF = unlimited stays above)
    *slice = ms;
    c_finalizeLowered.fetch_add(1, std::memory_order_relaxed);
    game(svc, edx, a, b);
    *slice = gameMs; // read only at 0x00599A20: the game's value is back before anything else sees it
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_on.load(std::memory_order_acquire)) return true;
    std::string missing;
    if (!GameAddr::GroupAvailable("LotBuildShare", &missing)) { // WorldManagerPtr; same game build as the lot gate
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }
    uint32_t slot = 0;
    if (!MemPatch::ReadBytes(kJobVtableSlot, &slot, sizeof(slot)) || slot != kJobUpdate ||!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kJobUpdate), kJobUpdateBytes, sizeof(kJobUpdateBytes))) {
        if (error) *error = "the JobManager update is not the expected code (another game version or another mod)";
        return false;
    }
    g_worldGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_msPerTick = 1000.0 / static_cast<double>(f.QuadPart);
    g_frameStart = 0;
    g_on.store(true, std::memory_order_release);
    const DWORD orig = static_cast<DWORD>(kJobUpdate);
    if (!MemPatch::WriteDWORD(kJobVtableSlot, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&JobUpdateHook)), &g_patches, &orig)) {
        g_on.store(false, std::memory_order_release);
        if (error) *error = "the JobManager update slot could not be written";
        return false;
    }
    LOG_INFO(std::format("[DeadlineSlicing] On: while the camera moves, deferrable work stops at {:.0f} ms into the frame; finalize slice {} ms", g_deadlineMs.load(),
                         g_finalizeMs.load()));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_on.load(std::memory_order_acquire)) return;
    g_on.store(false, std::memory_order_release); // the wrapper passes through from now on, even if the slot stays
    if (!MemPatch::RestoreAll(g_patches)) LOG_ERROR("[DeadlineSlicing] The JobManager update slot could not be put back; the wrapper stays and passes every call through");
    LOG_INFO("[DeadlineSlicing] Off (" + StatusText() + ")");
}

bool Running() { return g_on.load(std::memory_order_acquire); }

double FrameLeftMs() {
    if (!g_on.load(std::memory_order_relaxed) || !g_moving || !g_frameStart) return kNoDeadline;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const double elapsed = static_cast<double>(t.QuadPart - g_frameStart) * g_msPerTick;
    if (elapsed > 250.0) return kNoDeadline; // no JobManager update this frame (a loading screen, a pause): no anchor
    return g_deadlineMs.load(std::memory_order_relaxed) - elapsed;
}

void NoteSceneCut(bool skipped) { (skipped ? c_sceneSkip : c_sceneCut).fetch_add(1, std::memory_order_relaxed); }
void NoteLotCut(bool skipped) { (skipped ? c_lotSkip : c_lotCut).fetch_add(1, std::memory_order_relaxed); }

std::string StatusText() {
    if (!Running()) return "Off";
    return std::format("On: {} of {} frames moving; finalize slice lowered {}; scene drain shortened {} / past the deadline {}; lot builds shortened {} / waited {}",
                       c_movingFrames.load(), c_frames.load(), c_finalizeLowered.load(), c_sceneCut.load(), c_sceneSkip.load(), c_lotCut.load(), c_lotSkip.load());
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    float d = g_deadlineMs.load();
    if (ImGui::SliderFloat("Frame deadline##DlTarget", &d, 4.0f, 33.0f, "%.0f ms")) g_deadlineMs.store(d);
    int f = g_finalizeMs.load();
    if (ImGui::SliderInt("Finalize slice while moving##DlFin", &f, 0, 5, "%d ms")) g_finalizeMs.store(f);
    ImGui::TextWrapped("%s", ("Frame deadline while moving: " + StatusText()).c_str());
}

} // namespace DeadlineSlicing

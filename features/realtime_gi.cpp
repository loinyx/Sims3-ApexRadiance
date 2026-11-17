// Real-time global illumination: module skeleton and the feature "RealtimeGI" (see realtime_gi.h).
//
// Part of Apex Radiance. Credits: @loinyx

#include "realtime_gi.h"
#include "patch_base.h"
#include "apex_log.h"
#include "apex_version.h"
#include "build_flavor.h"
#include "d3d9_hooks.h"
#include <array>
#include <atomic>
#include <format>
#include <windows.h>

namespace RealtimeGI {
namespace {

constexpr const char* kLotsNames[] = {"active", "activeAndNeighbours", "all"};
constexpr const char* kCapNames[] = {"off", "class0", "hold"};
constexpr const char* kIndirectNames[] = {"off", "bounce1", "multiBounce"};
constexpr DWORD kLogEveryMs = 10000;

std::array<std::atomic<uint64_t>, static_cast<size_t>(Counter::Count)> g_counters{};
std::atomic<bool> g_running{false};
std::atomic<Backend> g_backend{Backend::D3d9};
uint64_t g_frames = 0; // render thread
DWORD g_lastLog = 0;

// shortcut: no bridge exports the GI interface yet, so the server back ends are never available; the server packages
// (R-02 onward) replace this with the export lookup in the bridge client d3d9.dll.
bool ServerExport() { return false; }
bool ServerRayQuery() { return false; }

// Developer mode: the counters every 10 s, with the frames since the last line
void OnPresent() {
    ++g_frames;
    const DWORD now = GetTickCount();
    if (g_lastLog == 0) g_lastLog = now;
    if (now - g_lastLog < kLogEveryMs) return;
    g_lastLog = now;
    const uint64_t frames = g_frames;
    g_frames = 0;
    if (kPublicBuild) return;
    const uint64_t cap = Value(Counter::CapSolvesOnGiLots), sync = Value(Counter::SyncReadbacks), locks = Value(Counter::ResourceLocks),
                   diffs = Value(Counter::RedrawDiffs);
    const std::string line = std::format("[RealtimeGI] Counters: frames={} backend={} draws={} deviceCalls={}; must be 0: capSolves={} syncReadbacks={} "
                                         "locks={} redrawDiffs={}",
                                         frames, kBackendNames[static_cast<int>(g_backend.load())], Value(Counter::Draws), Value(Counter::DeviceCalls), cap,
                                         sync, locks, diffs);
    if (cap || sync || locks || diffs) LOG_WARNING(line);
    else LOG_INFO(line);
}

} // namespace

void Add(Counter c, uint32_t n) { g_counters[static_cast<size_t>(c)].fetch_add(n, std::memory_order_relaxed); }
uint64_t Value(Counter c) { return g_counters[static_cast<size_t>(c)].load(std::memory_order_relaxed); }
bool Running() { return g_running.load(); }
Backend InUse() { return g_backend.load(); }

bool Start(const Options& o, std::string* error) {
    if (g_running.load()) return true;
    const Backend backend = SelectBackend(o.backend, ServerExport(), ServerRayQuery());
    g_backend = backend;
    g_frames = 0;
    g_lastLog = 0;
    using namespace D3D9Hooks;
    if (!RegisterPresent(kPatchName, [](DeviceContext&, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnPresent();
            return HookAction::Continue;
        })) {
        if (error) *error = "Could not register the Present callback";
        return false;
    }
    g_running = true;
    // read back by the test harness (design.md section 6); lots = GI lots covered now (the lamp snapshot, S1-02, adds them)
    LOG_INFO(std::format("[RealtimeGI] On: lots=0 cap={} scale={} indirect={} backend={}", kCapNames[o.solveCap], o.scale, kIndirectNames[o.indirect],
                         kBackendNames[static_cast<int>(backend)]));
    if (backend != o.backend && o.backend != Backend::Auto)
        LOG_INFO(std::format("[RealtimeGI] Back end {} is not available here; using {}", kBackendNames[static_cast<int>(o.backend)],
                             kBackendNames[static_cast<int>(backend)]));
    return true;
}

void Stop() {
    if (!g_running.load()) return;
    D3D9Hooks::UnregisterAll(kPatchName);
    g_running = false;
    LOG_INFO("[RealtimeGI] Off");
}

} // namespace RealtimeGI

namespace {

class RealtimeGiPatch : public ApexPatch {
  public:
    RealtimeGiPatch() : ApexPatch(RealtimeGI::kPatchName, nullptr) {
        // TOML keys: never rename (design.md section 5)
        RegisterNamedEnumSetting(&backend_, "backend", 0, "Back end: auto, serverRays, serverCompute or d3d9",
                                 {std::begin(RealtimeGI::kBackendNames), std::end(RealtimeGI::kBackendNames)});
        RegisterNamedEnumSetting(&lots_, "lots", 0, "Lots covered", {std::begin(RealtimeGI::kLotsNames), std::end(RealtimeGI::kLotsNames)});
        RegisterNamedEnumSetting(&solveCap_, "solveCap", 0, "The game's room solves on GI lots", {std::begin(RealtimeGI::kCapNames), std::end(RealtimeGI::kCapNames)});
        RegisterNamedEnumSetting(&scale_, "bufferScale", 0, "Light buffer scale", {"1", "2", "4"});
        RegisterNamedEnumSetting(&indirect_, "indirect", 0, "Indirect light", {std::begin(RealtimeGI::kIndirectNames), std::end(RealtimeGI::kIndirectNames)});
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        const RealtimeGI::Options o{lots_, solveCap_, 1 << scale_, indirect_, static_cast<RealtimeGI::Backend>(backend_)};
        if (!RealtimeGI::Start(o, &error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        RealtimeGI::Stop();
        isEnabled = false;
        lastError.clear();
        return true;
    }

  private:
    int backend_ = 0, lots_ = 0, solveCap_ = 0, scale_ = 0, indirect_ = 0;
};

} // namespace

APEX_REGISTER_FEATURE(RealtimeGiPatch, {.displayName = "Real-Time Lighting",
                                        .description = "Lamps light the lot through a light buffer Apex keeps up to date every frame, with light bouncing between "
                                                       "surfaces. In development: the switches are for testing only. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                        .category = "Graphics",
                                        .experimental = true,
                                        .enabledByDefault = false,
                                        .supportedVersions = VERSION_STEAM,
                                        .technicalDetails = {"Off: no hook and no resource. On: one Present callback (profiler line \"RealtimeGI\") and the "
                                                             "start line read back by the test harness."},
                                        .gameCodeGroup = nullptr});

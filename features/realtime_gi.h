#pragma once
// Real-time global illumination (Apex Radiance; docs/features/realtime-gi/design.md). Feature "RealtimeGI", TOML
// [patches.RealtimeGI], off by default; the keys are developer-mode only until the menu stage (design.md section 5).
//
// This is the module skeleton (work package S1-01): with the feature off nothing is hooked and no resource is created.
// On, it prints the start line read back by the test harness, registers one Present callback named "RealtimeGI" (the
// frame profiler times it by that name: the module's ms a frame) and writes the counters every 10 s in developer mode.
// The light work itself (lamp snapshot, solve cap, light buffers) comes with the later packages.
#include <cstdint>
#include <string>

namespace RealtimeGI {

inline constexpr const char* kPatchName = "RealtimeGI";

// [patches.RealtimeGI] backend
enum class Backend : int { Auto, ServerRays, ServerCompute, D3d9 };
inline constexpr const char* kBackendNames[] = {"auto", "serverRays", "serverCompute", "d3d9"};

// The back end used for a requested one: the server back ends need the bridge's GI export, serverRays also ray queries
// on the server's GPU; auto takes the best one available; anything unavailable falls back to d3d9 (design.md 3.9).
constexpr Backend SelectBackend(Backend requested, bool serverExport, bool rayQuery) {
    if (!serverExport) return Backend::D3d9;
    if (requested == Backend::ServerCompute) return Backend::ServerCompute;
    if (requested == Backend::D3d9) return Backend::D3d9;
    return rayQuery ? Backend::ServerRays : Backend::ServerCompute; // auto or serverRays
}
static_assert(SelectBackend(Backend::Auto, false, true) == Backend::D3d9);
static_assert(SelectBackend(Backend::Auto, true, true) == Backend::ServerRays);
static_assert(SelectBackend(Backend::Auto, true, false) == Backend::ServerCompute);
static_assert(SelectBackend(Backend::ServerRays, true, false) == Backend::ServerCompute);
static_assert(SelectBackend(Backend::ServerRays, false, true) == Backend::D3d9);
static_assert(SelectBackend(Backend::ServerCompute, true, true) == Backend::ServerCompute);
static_assert(SelectBackend(Backend::D3d9, true, true) == Backend::D3d9);

struct Options {
    int lots = 0;     // 0 active, 1 activeAndNeighbours, 2 all
    int solveCap = 0; // 0 off, 1 class0, 2 hold
    int scale = 1;    // bufferScale: 1, 2 or 4
    int indirect = 0; // 0 off, 1 bounce1, 2 multiBounce
    Backend backend = Backend::Auto;
};

// Counters of design.md section 6 the skeleton owns. The first four must stay 0; the later packages add to them.
enum class Counter : int {
    CapSolvesOnGiLots, // class 1-2 room solves on a GI lot (must be 0)
    SyncReadbacks,     // synchronous GetRenderTargetData from GI code (must be 0)
    ResourceLocks,     // locks of GI resources (must be 0)
    RedrawDiffs,       // debugRedrawAll differences (must be 0)
    Draws,             // draws issued by GI code
    DeviceCalls,       // other device calls by GI code
    Count
};
void Add(Counter c, uint32_t n = 1); // any thread
uint64_t Value(Counter c);

bool Start(const Options& options, std::string* error); // render thread or pump thread; prints the start line
void Stop();
bool Running();
Backend InUse(); // the selected back end while running

} // namespace RealtimeGI

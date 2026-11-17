#pragma once
// Real-time global illumination (Apex Radiance; docs/features/realtime-gi/design.md). Feature "RealtimeGI", TOML
// [patches.RealtimeGI], off by default; the keys are developer-mode only until the menu stage (design.md section 5).
//
// This is the module skeleton (work package S1-01): with the feature off nothing is hooked and no resource is created.
// On, it prints the start line read back by the test harness, registers one Present callback named "RealtimeGI" (the
// frame profiler times it by that name: the module's ms a frame) and writes the counters every 10 s in developer mode.
// S1-02 adds the lamp snapshot: every light of the world, read on the render thread (below). The solve cap and the light
// buffers come with the later packages.
#include "gpu_lamp_law.h"
#include <cstdint>
#include <string>
#include <vector>

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
    bool debugLampTable = false; // developer mode: dump the snapshot against the game's fields after each change
    bool pixelShadows = false;   // per-pixel delta shadows through the bridge (gi_pixel_shadows.h)
    int pixelView = 0;           // their developer view: 0 off, 1 filtered mask, 2 raw mask
    bool pixelTestCaster = false; // developer: one capsule under the strongest lamp (until XR-08)
    bool meshCasters = false;    // developer mode: objects to the bridge server's mesh casters (wp/MC.md, MC-2)
    bool debugFakeSolve = false; // developer mode: the layer update scheduler (gi_sched.h, S2-01) on its fake back end
};

// Counters of design.md section 6 the skeleton owns. The first four must stay 0; the later packages add to them.
enum class Counter : int {
    CapSolvesOnGiLots, // class 1-2 room solves on a GI lot (must be 0)
    SyncReadbacks,     // synchronous GetRenderTargetData from GI code (must be 0)
    ResourceLocks,     // locks of GI resources (must be 0)
    RedrawDiffs,       // debugRedrawAll differences (must be 0)
    Draws,             // draws issued by GI code
    DeviceCalls,       // other device calls by GI code
    ReadFaults,        // SEH faults in the light enumeration or a lamp read (must be 0)
    LampTableDiffs,    // debugLampTable differences against the game's fields and Apex's lamp table (must be 0)
    LampChanges,       // lamp change events committed
    Count
};
void Add(Counter c, uint32_t n = 1); // any thread
uint64_t Value(Counter c);

// ---- lamp snapshot (S1-02; research/01-lamp-data.md) ----
// One light as the GI takes it. Read on the render thread from every light the game enumerates (0x006ACF70: indoor lamps
// are in no light cell, so the cells would miss them); `light` is an identity only, never read in a later frame.
struct Lamp {
    uintptr_t light = 0;
    uint64_t lot = 0;                  // +0xC0 (0: a world light)
    int type = 0, room = 0, story = 0; // +0xB0 (3..11), +0x08 (0 outdoors), +0xD0
    uint8_t flags = 0;                 // +0x100: 0x01 alive, 0x20 lit, 0x40 enabled
    bool on = false;                   // alive, enabled, lit, dimmer and fade over 0; a change of it is taken on its second read
    float pos[3] = {}, base[3] = {};   // +0x120 head, +0xF0 colour
    float dimmer = 0, fade = 0, range = 0; // +0x10, +0x20, +0x130
    float colour[3] = {};              // base x dimmer x fade while on, else 0
    // the raw cone: spot (4) axis +0x170, off1 +0x158, scale +0x154; shaded lamp (5) a1 +0x1A0, o1 +0x174, o2 +0x170, spill +0x150
    float axis[3] = {}, off1 = 0, off2 = 0, scale = 0, spill[3] = {};
    bool hasLaw = false;               // a lot lamp with a GPU law (RtOutdoorLamps::OwnLotLaw: classes 3-6, 9-11)
    GpuLampLaw::Lamp law;
};
enum class LampChange : uint8_t { Added, Removed, Switched, Moved, Value }; // (GiSched::Event takes the same order)
inline constexpr const char* kLampChangeNames[] = {"added", "removed", "switched", "moved", "value"};
struct Change {
    uintptr_t light = 0;
    uint64_t lot = 0;
    LampChange kind = LampChange::Value;
};
// The light tree thread rewrites the lit bit under the reader: a new on state is taken when two reads (one frame apart at
// least) agree, else the last taken one stays
constexpr bool DebounceOn(bool taken, bool lastRead, bool now) { return now == lastRead ? now : taken; }
static_assert(DebounceOn(false, true, true) && !DebounceOn(false, false, true) && DebounceOn(true, true, false) && !DebounceOn(true, false, false));
// Render thread: the snapshot (empty outside a world), its serial (grows at each committed change), the changes of the
// last commit, and the GI lots (the `lots` setting over the snapshot)
const std::vector<Lamp>& Lamps();
uint32_t LampSerial();
const std::vector<Change>& Changes();
int GiLots();

bool Start(const Options& options, std::string* error); // render thread or pump thread; the start line follows in a world
void Stop();
bool Running();
Backend InUse(); // the selected back end while running

} // namespace RealtimeGI

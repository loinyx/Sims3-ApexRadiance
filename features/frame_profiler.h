#pragma once
// Frame-hitch profiler (Apex Radiance). Measures, on the player's machine, what each frame and each
// hitch is made of: frame time from the Present hook, timed wrappers around the game functions suspected of hitching
// (lot streaming / lot LOD, lot lighting, terrain, script GC, the game's render and present path, the frame limiter),
// D3D9 call counts and the time the mod's own D3D9 registry hooks take per frame. A hitch is a frame longer than
// max(multiplier x median of the last 120 frames, floor). Results: live UI (graph, percentiles, per-category breakdown of
// the last hitches) and ApexRadiance_Hitches.txt in the Apex Radiance folder, appended in batches at most once per second from a
// background thread.
//
// Optional statistical sampling (Advanced): a sampler thread records where the render / simulation thread is (EIP, TS3W
// call sites on its stack) ~2000 times a second, so each hitch also shows which code its untimed time was spent in.
//
// Counters (research\perf2\plan.md section 8): resource lookups (FindProvider), scene pending nodes, RefPack compression,
// DXT encoding, object lookups by ID and lot room solves, each with calls / ms per thread bucket, the longest call and an
// extra count; every hitch also names its dominant cause. Two measurement presets set the recommended options.
//
// Off (the default) = nothing is hooked: no detours, no registry hooks, no thread. See frame_profiler.cpp for the
// verified addresses, the timing rules (exclusive / inclusive, thread buckets) and the known limitations.
#include <cstdint>
#include <string>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

namespace FrameProfiler {

// Mod time measured outside the profiler's own hooks (development build; no-ops in the public build):
//  - D3DDispatch: the registry times its outermost dispatch of every chain but Present (framework/d3d9_hooks.cpp),
//    booked as "D3D hooks (mod)", also when a callback returns Skip / Block;
//  - LampRefresh: Night Lighting's lamp list refresh (every 20 frames, lot_light_bridge.cpp), "Lamp refresh (mod)";
//  - ReissuedDraw: the driver call of a draw Night Lighting re-issued (framework/d3d9_hooks.cpp TimedReissueDraw), inside
//    a D3DDispatch sample, "Re-issued draw (driver)": the game would have paid it for its own call.
enum class ModTime : int { D3DDispatch, LampRefresh, ReissuedDraw };


// Turns the profiler on (registers the D3D9 hooks, starts the file writer; the timed game functions are resolved and
// attached at the next frame boundary, on the render thread) or off (detaches and unregisters everything; the collected
// statistics stay visible until Clear). Thread-safe.
void SetEnabled(bool on);
bool IsEnabled();

// The profiler section of the menu (checkbox, live line, frame-time graph, percentiles, last hitches, Advanced).
void RenderUI(bool showEnable = true); // showEnable = false: the caller draws the on/off switch

// Settings under [qol.frame_profiler]
void SaveToToml(toml::table& qolTable);
void LoadFromToml(const toml::table& qolTable);

// Detaches everything (call before the D3D9 hooks are cleaned up at shutdown). Same as SetEnabled(false).
void Shutdown();

// ---- per-hook timing in the D3D9 hook registry (framework/d3d9_hooks.cpp) ----
// The registry times each draw-call callback while RegistryHookTimingActive() (a relaxed atomic load, true only while the
// profiler is on and the Advanced option "Per-hook registry timing" is checked). (2.15.3) Each callback's own time: the
// callbacks nested in it (a re-issued draw's chain) and the driver time of the draws issued inside it are left out, so
// a callback that draws in the game's place is not charged for the draw the game would have made. Every registered name
// gets a slot once (RegistryTimingSlot, at registration); AddRegistryHookTimeAt adds to it with relaxed atomics, any thread.
bool RegistryHookTimingActive();
uint64_t Ticks();
// The actual device allocation inside a measured DDS load, excluding Apex callbacks.
uint64_t BeginTextureAllocation();
void EndTextureAllocation(uint64_t start, bool succeeded);
int RegistryTimingSlot(const std::string& hookName); // -1 when the table is full
void AddRegistryHookTimeAt(int slot, uint64_t ticks);
// Present chain: every callback is timed by name while the profiler is on (a relaxed atomic load); the registry reports
// it through AddRegistryHookTime under the name "<name> (Present)".
bool PresentHookTimingActive();

// ---- mod time scopes (see ModTime) ----
// True while the profiler is on (a relaxed atomic load). BeginModTime pushes a timed frame on the calling thread (key =
// an address in the caller's stack frame) and returns a token, -1 when nothing was pushed (profiler off, no thread slot);
// EndModTime(token, key) closes it. Nesting follows the usual self / inclusive rules.
bool ModTimeActive();
int BeginModTime(ModTime what, const void* key);
// A sample standing for `scale` calls: its time and the time of every frame pushed inside it are booked x scale, and its
// parent loses the scaled time (the registry times one outermost dispatch in 16). Closed by EndModTime. -1 inside a sample.
int BeginModTimeSampled(ModTime what, const void* key, uint32_t scale);
void EndModTime(int token, const void* key);

// RAII form for a scope on one thread
class ModTimeScope {
  public:
    explicit ModTimeScope(ModTime what) : token_(ModTimeActive() ? BeginModTime(what, this) : -1) {}
    ~ModTimeScope() {
        if (token_ >= 0) EndModTime(token_, this);
    }
    ModTimeScope(const ModTimeScope&) = delete;
    ModTimeScope& operator=(const ModTimeScope&) = delete;

  private:
    int token_;
};



} // namespace FrameProfiler

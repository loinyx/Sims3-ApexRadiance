#pragma once
// What Apex was doing when a session ended without closing. Credits: @loinyx
// A GPU hang (a driver fault: under the RTX Remix bridge the server loses its device, VK_ERROR_DEVICE_LOST) freezes the game
// with no exception: no crash report, the log just stops, and the player ends TS3W.exe from the Task Manager. Apex keeps
// its last 256 steps on the render thread (each HookGuard step, each post-scene effect, each Looks shader, each Present; a step repeated at once
// is one entry with a count) in Apex Radiance\ApexRadiance_Trace.bin, a mapped file Windows writes out even when the
// process is ended. At the next start, a session that did not close is reported in the log: its last steps, the graphics
// driver's errors since it started (Windows' System log: nvlddmkm, amdkmdag, igfx, Display, TDRs) and the RTX Remix
// bridge's log of that session (Game\Bin\NvRemixBridge_d3d9.log), when the bridge has not written over it yet.

#include <atomic>
#include <cstdint>

namespace GpuTrace {

void Start() noexcept;                // the init thread, after the log opens: reports the last session, starts this one's
void Mark(const char* step) noexcept; // any thread; step is copied (first 47 characters)
void Frame() noexcept;                // after each Present (render thread)

// Set by Start(); null in the offline tools that share Apex's sources (they do not link gpu_trace.cpp)
inline std::atomic<void (*)(const char*) noexcept> g_mark{nullptr};
inline std::atomic<volatile uint32_t*> g_state{nullptr}; // 1 running, 2 closed, 3 crash report written
// Mark when the trace runs (HookGuard, the post-scene effects, Looks)
inline void MarkIfOn(const char* step) noexcept
{
    if (auto f = g_mark.load(std::memory_order_relaxed)) f(step);
}
// The process ends normally (DLL_PROCESS_DETACH): this session closed
inline void Closed() noexcept
{
    if (volatile uint32_t* s = g_state.load(std::memory_order_relaxed); s && *s == 1) *s = 2;
}
// A crash report was written: not reported again as a hang
inline void Crashed() noexcept
{
    if (volatile uint32_t* s = g_state.load(std::memory_order_relaxed)) *s = 3;
}

} // namespace GpuTrace

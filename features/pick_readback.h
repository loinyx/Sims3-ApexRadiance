#pragma once
// The game reads its mouse pick target back from the GPU every other frame (GetRenderTargetData), and the copy waited
// for the GPU to finish the frame. The read-back now waits a frame or two until an event query says the GPU is past
// the pick's draws, so the copy no longer stalls the render thread. Steam 1.67.2 only; always on.
//
// Feature "PickSkipWhenStill" (off by default; docs/features/performance/pick-skip-when-still.md): the game re-picks at
// the last cursor position every other frame even when nothing moves. That re-pick waits while the camera, the viewport and
// the pick modes are as they were at the last pick (at most 250 ms), and while the camera moves only every 4th update
// picks (mouse-move requests too). A button, wheel or key press or release always lets the next update pick.
#include <cstdint>
#include <string>
namespace PickReadback {
void Install();          // after the features start (the device hooks come later; the first Present gives the device)
bool Installed();        // the update is hooked (Steam 1.67.2, its code as studied)
void SetSkipWhenStill(bool on); // any thread
std::string StatsText(); // developer log
// Since the last call (render thread, the Frame Profiler's timeline): picks rendered, updates skipped with nothing changed,
// updates skipped by the camera-motion cap, read-backs made and their time (QPC ticks)
void TakeCounters(uint32_t& runs, uint32_t& still, uint32_t& moving, uint32_t& readbacks, int64_t& readbackTicks);
}

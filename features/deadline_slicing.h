#pragma once
// Frame deadline while moving (Apex Radiance, feature "DeadlineSlicing", menu "Frame deadline while moving"; experimental,
// off by default; docs/features/performance/deadline-slicing.md).
//
// While the camera moves, a frame that already holds one long item (a texture encode, a lot's first load stage, a Sim
// build step) used to run the deferrable work of the frame beside it: the scene's pending objects (Spread new objects
// over frames, 2 ms), the load stages of streaming lots (Shared lot build time, 4 ms) and the main-thread finalize jobs of
// the resource loader (the game's 5 ms slice). Each of them now gets only the time left until a frame deadline (12 ms
// after the frame's services started): a frame past the deadline leaves that work for the next frame. Nothing is cut:
// the longest-wait rules of those features stay (objects 500 ms then a growing budget, lots 6 frames).
//
// The finalize jobs run first in the frame (JobManager is the first time-sliced service), so a deadline cannot see the
// long item that comes after them; while moving their slice is lowered from the game's 5 ms to 2 ms instead (one job
// always runs, as in the game). The JobManager update's vtable slot (0x00FD9810 + 0x1C -> 0x00599A10 on Steam 1.67.2)
// is redirected to a wrapper that marks the frame start, writes the slice ([svc+0xCC], read at 0x00599A20) and puts the
// game's value back after the call. Render thread.
#include <cstdint>
#include <string>

namespace DeadlineSlicing {

bool Start(std::string* error);
void Stop();
bool Running();

// Render thread: ms left before this frame's deadline while the feature is on and the camera moves (<= 0: past it);
// kNoDeadline otherwise
inline constexpr double kNoDeadline = 1e9;
double FrameLeftMs();
// Callers report what the deadline took from them (counters only)
void NoteSceneCut(bool skipped);
void NoteLotCut(bool skipped);

std::string StatusText();
void RenderDeveloperUI();

} // namespace DeadlineSlicing

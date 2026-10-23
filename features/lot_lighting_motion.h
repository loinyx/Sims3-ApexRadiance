#pragma once
// Lot lighting while the camera moves (Apex Radiance, feature "LotLightingMotion"; docs/features/performance.md).
//
// Every frame each lot with lighting work runs the lot lighting update (0x00ADB8F0 on Steam 1.67.2), which asks
// 0x00ADB120 for its time budget (ms, returned in ST0: 5 for an ordinary lot, 10 while it loads, 15 for a "priority"
// lot, 30 while that one loads, 1000 in the world tool mode) and relights its levels' rooms until the budget is spent
// (the room solve resumes where it stopped on the next frame). While the camera moves the current lot's 15 ms made the
// medium hitches. This feature redirects the only CALL of 0x00ADB120 (0x00ADB95D) to a wrapper that, while the camera
// moves, scales the game's budget so the priority lot gets `budgetMs` (default 3) and every other lot the same fraction
// of its own budget (5 -> 1 ms at 3 ms): the engine's own order and priorities stay, only the time per frame shrinks.
// The world tool mode's 1000 ms is left alone. When the camera stops (300 ms without motion) the game's budgets return.
// Camera motion: the camera eye ([[root]+0x24]+0x60, the same read WorldManager::Update does at 0x00C6D5C9), sampled at
// each budget call and once per frame (Present) while this feature or the wall shading gate below is on, and at each
// Scene::BeginFrame drain while the scene node budget (features/scene_budget.h) is on (SampleCameraMoving).
#include <string>

namespace LotLightingMotion {

// Redirects the CALL (feature on) / puts it back (feature off). Any thread.
bool Start(std::string* error);
void Stop();
bool Running();

// The priority lot's budget while the camera moves, in ms (1..15; 15 = the game's own)
void SetBudgetMs(int ms);
int BudgetMs();
// For the next `ms` milliseconds every lot keeps the game's own lighting budget even while the camera moves (a lot lamp
// was switched on or off: its rooms relight at once instead of over many frames). Any thread.
void Boost(unsigned ms);

// The camera moved in the last 300 ms (false when unknown or when both this feature and the wall shading gate are off)
bool CameraMoving();
// Samples the camera eye now and returns whether it moved in the last 300 ms, whether or not this feature is on (the
// camera offsets are parsed from the game's code on the first call; false while they are unknown). Any thread; called
// once per frame by the scene node budget (features/scene_budget.h). Same motion test as the rest of this file (eye change
// > 5 mm between two samples, or within 100 ms for slow pans, held 300 ms); its samples also feed the lot budget and the
// wall shading gate when those are on.
bool SampleCameraMoving();
// The camera eye now (world); false while its location in the game's data is unknown. Any thread.
bool CameraEye(float out[3]);

// The flush after a lamp drag (DragLite::SolveCapMs): Faster room lighting's drain takes what it needs up to the spread cap
// (DrainBudget) and books what it spent (NoteDrainSpent, 0 at each pick); the lot being played gets the rest of the cap
float DrainBudget(float wanted); // the drain's budget this frame (render thread)
// (2.15.1) 0..1: how much of the settle window's raised solving budgets the frame time leaves (1 without the Present sampler)
float SettleScale();
float BaselineFrameMs(); // the frame time outside the settle window (ms; 0 = not measured yet)
// (2.15.3) The steady pacing (camera still, PerfToggles::SteadyRoomPacing): 0..1, how much of the game's room solving budget
// the frame time leaves (1 when the frames hold, or without the Present sampler); and the drain budget it gives from `wanted`
float SteadyScale();
float SteadyDrainMs(float wanted);
void NoteDrainSpent(float ms);   // what the drain spent this frame (render thread)

std::string StatusText();
// Development build: status lines
void RenderDeveloperUI();

// ---- Wall shading while moving (feature "WallShadingWhileMoving"; docs/features/performance.md) ----
// Each lot level has a wall ambient-occlusion solver (level+0x290, vtable 0x00FF0594 on Steam 1.67.2) whose step
// 0x0068B810 (reached only through the slot 0x00FF05B0) shades every outdoor wall of the level in one go, with no time
// check: 10-17 ms per pass, and several passes in one frame when lots load (54-108 ms). The step is called by the solver
// driver 0x00688920 (slot +0xC), which stores the step's return value as the solver state (0 first pass due, 1 refinement
// due, 2 done) and calls it again next frame while the state is not 2. Returning the current state is therefore "not
// done, try later", which the step itself does in state 1 when its cost estimate is negative. This gate swaps the slot
// (framework/slot_chain.h) and, for calls from that driver with a per-frame budget (< 100 ms: the synchronous solve's
// 60 s and the tool mode's 1000 ms are never touched):
//   - while the camera moves: defers the first passes (state 0) and the refinements (state 1) until it stops, with one wait
//     of at most `firstPassWaitMs` (default 2 s) per state and per camera motion (the lot's load finishes only after the
//     first pass of all its levels (0x00ADBBA0 at load stage 20), and a lot thumbnail (ThumbnailManager, 0x00AE06B0 ->
//     0x00ADBC30) only after every refinement). Once that wait ran out, the pending passes of that state run one per frame
//     while the camera keeps moving; both waits start over when the camera is seen still (at a Present);
//   - always: at most one pass that takes >= 1 ms per frame across all lots (a frame = between two Presents, or 33 ms).
// The gate is the outermost layer of the slot (SlotChain::Layer::Gate): it needs its own return address to recognise the
// driver, and the Frame Profiler's "Wall AO pass" counter (inner layer) then times only the passes that run.
bool StartWallAo(std::string* error);
void StopWallAo();
bool WallAoRunning();
std::string WallAoStatusText();
void RenderWallAoDeveloperUI();

} // namespace LotLightingMotion

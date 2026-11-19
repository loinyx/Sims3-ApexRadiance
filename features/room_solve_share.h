#pragma once
// Shared room solve time while moving (option of Lot Lighting While Moving, `[patches.LotLightingMotion] sharedSolveTime`,
// off by default; docs/features/performance/lot-lighting-motion.md "Shared room solve time").
//
// Each frame the lot pass runs the lot lighting update 0x00ADB8F0 of every lot with lighting work, and each update solves
// its levels (one call of 0x006A8BA0 per level, the CALL at 0x00ADB9AD) until its own budget is spent, with at least one
// level per lot. Lot Lighting While Moving shrinks each lot's budget, but every lot still runs at least one level, so a
// pan over many lots adds the levels up: about 27 solves and 9 ms per hitch frame, up to 60 ms in one. This gate sits on
// the CALL 0x00ADB9AD (framework/call_chain.h, outermost layer) and, while the camera moves, gives all lots one solve
// budget per frame (room_solve_share_policy.h): a level whose turn comes once it is spent is not solved this frame (the
// call is not made; its rooms keep their state and resume when it runs), the lot being played always runs, and no level
// waits more than RoomSolveSharePolicy::kHardMaxSkips frames. Only calls inside the lot pass are gated (a layer on its
// CALL 0x00C7CEDF marks them; the impostor builder's own updates pass through). With the camera still every call runs.
#include <cstdint>
#include <string>

namespace RoomSolveShare {

bool Start(std::string* error);
void Stop();
bool Running();

// Level solves that waited a frame, since the start (any thread), and the most frames a level is waiting now (0 when off)
uint32_t WaitedTotal();
uint32_t LongestWaitNow();

std::string StatusText();
void RenderDeveloperUI();

} // namespace RoomSolveShare

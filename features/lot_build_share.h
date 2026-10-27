#pragma once
// Shared lot build time (started with Lot Lighting While Moving; docs/features/performance/lot-streaming.md "Shared lot
// build time"; development switch lot_build_share).
//
// The lot pass 0x00C7CEA0 updates every lot renderer each frame (0x00AEB2E0), and a renderer that is still loading runs
// its load stages 0x00AEA680 with its own budget: 20 ms, 35 for the lot being played (6 while the camera moves, from Lot
// Lighting While Moving). Lots that stream in together therefore cost one slice each in the same frame: recorded loads
// show up to 7 lots building in one frame and frames of 30 to 130 ms dominated by "Lot load stages". This gate sits on
// the only CALL of 0x00AEA680 (0x00AEB306, framework/call_chain.h, outermost layer) and gives the lots one budget per
// frame (lot_build_share_policy.h): each call's slice is what is left of it (passed to the slice stub of Lot Lighting
// While Moving through CapSlice), and a lot whose turn comes once the budget is spent waits for a later frame by
// returning 0 without calling the game, exactly as the stages do when their lot's lock is busy (the renderer update
// ignores the result). Only calls from the lot pass are gated (the impostor builder's synchronous updates and the
// loading screen's 2000 ms pass through), the lot being played always runs, and the waiting stages (17 on) are never held.
#include <cstdint>
#include <string>

namespace LotBuildShare {

bool Start(std::string* error);
void Stop();
bool Running();

// Inside the load stages (the slice stub, render thread): the slice this call may use, given the game's `gameMs`
uint32_t CapSlice(uint32_t gameMs);

std::string StatusText();

} // namespace LotBuildShare

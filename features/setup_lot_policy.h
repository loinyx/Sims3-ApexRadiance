#pragma once
// Decision of the lot capture test (level_light_share.cpp, SetupOnlyLot), without game state: tools/setup_lot_test checks it.
//
// During a New Game the game lights about 11 lots at the high quality, one after another, only to take their pictures
// (household setup). Such a lot has the high quality (story manager +0x288) while it is not the world's active lot
// (WorldManager +0x170/+0x174, read by the getter 0x00C5FB00) and not the game's priority lot. The played lot never
// qualifies: its id is the world's active id, or the game marks it as the priority lot.
// Unverified while the game runs: that the story manager's lot id (+0x90/+0x94) and the world's active id agree on the
// played lot. Such a lot skips light sharing (level_light_share.cpp, fastLotPictures).
#include <cstdint>

namespace SetupLotPolicy {

// getterOk: the world's active lot getter has its expected bytes; priority: the game marks the lot as its priority lot
// (true when that cannot be told)
inline bool Gate(bool allLotsHigh, bool getterOk, int quality, uint64_t lotId, uint64_t worldActiveId, bool priority) {
    return !allLotsHigh && getterOk && quality != 0 && lotId != 0 && lotId != worldActiveId && !priority;
}

// A lot gated before and no longer gated now gets one full re-solve. Keyed by lot id: a tracker reused for another lot
// does not inherit the previous lot's state.
inline bool ResolveDue(uint64_t prevLotId, bool prevGated, uint64_t lotId, bool gatedNow) {
    return prevGated && prevLotId == lotId && !gatedNow;
}

} // namespace SetupLotPolicy

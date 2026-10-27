#pragma once
// Decision of the shared lot build time (lot_build_share.h), without game state: tools/lot_streaming_test checks it.
//
// Each frame the load stages of every streaming lot (0x00AEA680, per lot, 20 ms; 35 for the lot being played) share one
// budget: kStillMs with the camera still, kMovingMs while it moves. A lot's call gets what is left of it as its slice; the
// lot being played always runs with the game's own slice (its time still counts); another lot whose turn comes with less
// than kRunFloorMs left waits for a later frame (the game's own "stage busy" answer: return 0, no state change); once it
// has waited kMaxSkips frames and longest of the waiting lots it runs one stage, one such lot per frame (several waiting
// lots forced together would rebuild the burst; forcing the longest waiter keeps every lot's wait bounded). Stages
// from kFirstWaitStage on only wait for a job or for the lot's lighting (about 1 us a call): they always run, so a lot
// never finishes later because of the share.
#include <cstdint>

namespace LotBuildSharePolicy {

inline constexpr double kStillMs = 6.0;
inline constexpr double kMovingMs = 4.0;
inline constexpr double kRunFloorMs = 1.0;
inline constexpr int kMaxSkips = 6;
inline constexpr int kFirstWaitStage = 17; // 17 and 18 post and wait for a job, 19 waits for 0x00C4D990, 20 for the lighting, 21 done
inline constexpr int kLastStage = 21;

struct Decision {
    bool run = true;
    uint32_t sliceMs = 0; // 0 = the game's own slice
    bool forced = false;  // runs only because it waited kMaxSkips frames
};

// longestWait: the most frames any lot had waited when this frame began
inline Decision Decide(double spentMs, bool moving, bool priority, int stage, int skips, bool forcedThisFrame = false, int longestWait = 0) {
    if (stage < 0 || stage >= kFirstWaitStage || priority) return {};
    const double left = (moving ? kMovingMs : kStillMs) - spentMs;
    if (left >= kRunFloorMs) return {true, static_cast<uint32_t>(left)}; // floor, at least 1
    if (skips >= kMaxSkips && skips >= longestWait && !forcedThisFrame) return {true, 1, true};
    return {false, 0};
}

} // namespace LotBuildSharePolicy

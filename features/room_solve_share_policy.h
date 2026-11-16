#pragma once
// Decision of the shared room solve time (room_solve_share.h), without game state: tools/room_solve_share_test checks it.
//
// While the camera moves, every lot level's room solve (0x006A8BA0, one call per level of each lot with lighting work, from
// the lot lighting update 0x00ADB8F0) shares one budget per frame, kMovingMs. A level whose turn comes with less than
// kRunFloorMs left waits for a later frame (its call is not made: the rooms keep their state and resume when it runs). The
// lot being played always runs (its time still counts). A waiting level runs anyway once it has waited kMaxSkips frames
// and longest of the waiting levels, one such level per frame, and in any case once it has waited kHardMaxSkips frames,
// so no level waits longer than that. With the camera still nothing waits.
#include <cstdint>

namespace RoomSolveSharePolicy {

inline constexpr double kMovingMs = 4.0;
inline constexpr double kRunFloorMs = 0.5;
inline constexpr int kMaxSkips = 4;
inline constexpr int kHardMaxSkips = 30;

struct Decision {
    bool run = true;
    bool forced = false; // runs only because it waited
};

// spentMs: the room solve time of this frame so far; skips: frames this level has waited; longestWait: the most frames any
// level had waited when this frame began; forcedThisFrame: a waiting level already ran this frame because it waited
inline Decision Decide(double spentMs, bool moving, bool priority, int skips, bool forcedThisFrame, int longestWait) {
    if (!moving || priority) return {};
    if (kMovingMs - spentMs >= kRunFloorMs) return {};
    if (skips >= kHardMaxSkips) return {true, true};
    if (skips >= kMaxSkips && skips >= longestWait && !forcedThisFrame) return {true, true};
    return {false, false};
}

} // namespace RoomSolveSharePolicy

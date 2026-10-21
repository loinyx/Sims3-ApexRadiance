#pragma once
#include <cmath>
#include <cstdint>

namespace RoomAmbientPolicy {
inline bool StructureRefreshDue(std::uint32_t now, std::uint32_t last) {
    return !last || static_cast<std::uint32_t>(now - last) >= 250;
}
inline bool FloorEditReady(std::uint32_t now, std::uint32_t last) {
    return last && static_cast<std::uint32_t>(now - last) >= 250;
}
inline bool AfterLoadRefreshReady(std::uint32_t now, std::uint32_t started, bool busy, std::uint32_t& quiet) {
    if (static_cast<std::uint32_t>(now - started) >= 8000) return true;
    if (busy) quiet = 0;
    else if (!quiet) quiet = now;
    return quiet && static_cast<std::uint32_t>(now - quiet) >= 250;
}
inline bool GroupWaitExpired(std::uint32_t now, std::uint32_t armed) {
    return static_cast<std::uint32_t>(now - armed) >= 1500;
}
// The played lot first, its camera's story first. With every floor detailed the stories above the camera come after the
// camera's and the ones below (1000 instead of 4000: a logged edit spent 84% of its solves on hidden stories, interleaved
// with the camera's), still before every other lot's rooms; each is still solved at its own class.
inline float FloorPriorityFactor(bool priorityLot, bool allFloors, int level, int camera) {
    if (!priorityLot) return 1.0f;
    if (level == camera) return 4000.0f;
    if (level < camera) return 2000.0f;
    return allFloors ? 1000.0f : 1.0f;
}
inline bool RetainFreshSolve(int state) { return state == 2 || state == 3 || state == 5; }
inline bool AmbientMapsCompatible(float norm, float nextNorm, float base, float nextBase) {
    return std::isfinite(norm) && std::isfinite(nextNorm) && std::isfinite(base) && std::isfinite(nextBase)
        && norm > 0 && nextNorm > 0 && std::fabs(norm - nextNorm) <= 1e-5f && std::fabs(base - nextBase) <= 1e-5f;
}
inline bool RigFallbackDue(std::uint32_t now, std::uint32_t armed, bool sent) {
    return !sent && static_cast<std::uint32_t>(now - armed) >= 1500;
}
// 12/10: a move waits 200 ms, the same quiet time that ends a drag (LevelLightShare kDragQuietMs), not 300
inline std::uint32_t LampRefreshDelay(bool switchOnly) { return switchOnly ? 120u : 200u; }
// 12/10: the safety net of a lamp moved into another room or story, or of many lamps, may run again after 500 ms (the
// whole lot is sent, and a drag across rooms should not wait 2 s for its last position); a switch keeps 2 s
inline std::uint32_t LampLotGap(bool relocatedOrAll) { return relocatedOrAll ? 500u : 2000u; }
// 12/10: an edit's room stays urgent while it still waits for or is in its solve (states 1-3), at most 8 s after its
// latest mark (a room of a paused lot must not keep the scheduler's first place for ever)
inline bool UrgencyKept(std::uint32_t now, std::uint32_t until, std::uint32_t marked, int state) {
    if (static_cast<std::int32_t>(now - until) < 0) return true;
    return state >= 1 && state <= 3 && static_cast<std::uint32_t>(now - marked) < 8000u;
}
// 12/10, lamp edits all at once (AtriumHold): 0 = keep holding, 1 = complete (every other story solved since the edit and
// the ground's chunks written), 2 = timeout (2.5 s after the latest edit, 4 s after the first). A drag is never decided
// while it goes on; its first edit is its release (the caller restarts `sinceFirst` then), so it times out 2.5 s after it.
inline int EditHoldOutcome(int pending, bool groundPending, std::uint32_t sinceLatest, std::uint32_t sinceFirst, bool dragging) {
    if (dragging || sinceLatest < 150u) return 0; // the game's marks and the safety net send the rooms first
    if (pending <= 0 && !groundPending) return 1;
    return sinceLatest >= 2500u || sinceFirst >= 4000u ? 2 : 0;
}
// 14/10, Classic: show all floors together (AtriumHold): every room of the edit, the lamp's own story's too, is counted in
// pending; the only bound is maxMs from the hold's first edit (a drag: its release), never during a drag
inline int EditHoldOutcomeTogether(int pending, bool groundPending, std::uint32_t sinceLatest, std::uint32_t sinceFirst, bool dragging, std::uint32_t maxMs) {
    if (dragging || sinceLatest < 150u) return 0;
    if (pending <= 0 && !groundPending) return 1;
    return sinceFirst >= maxMs ? 2 : 0;
}
inline bool GatherAfterChange(std::uint32_t started, std::uint32_t changed) {
    return static_cast<std::int32_t>(started - changed) > 0;
}
inline bool WindowRecheckDue(std::uint32_t now, std::uint32_t armed, unsigned pass) {
    return pass < 3 && static_cast<std::uint32_t>(now - armed) >= (pass == 0 ? 0u : pass == 1 ? 2000u : 6000u);
}
inline float MoveBackground(float own, float oldBase, float newBase) { return own + newBase - oldBase; }
inline void AccumulateAmbient(float* total, const float* own, float ownNorm, float groupNorm, float area) {
    const float scale = ownNorm > 1e-6f ? groupNorm / ownNorm : 1.0f;
    for (int k = 0; k < 4; k++) total[k] += own[k] * (k < 3 ? scale : 1.0f) * area;
}
inline float BackgroundShare(float brightness, float night) {
    return 1.0f + (brightness - 1.0f) * night;
}
inline bool SameRgb(const float* a, const float* b) {
    for (int k = 0; k < 3; k++)
        if (!std::isfinite(a[k]) || !std::isfinite(b[k]) || std::fabs(a[k] - b[k]) > 2e-4f) return false;
    return true;
}
inline bool SolverOwnsAmbient(int state) { return state >= 1 && state <= 3; }
inline bool AmbientUpdateDue(std::uint32_t now, std::uint32_t last) {
    return !last || static_cast<std::uint32_t>(now - last) >= 3000;
}
// Unknown or busy rooms are queued only after a change settles, never every scan.
inline bool NeedsAmbientSolve(int matchState, bool rest) { return rest && (matchState == 0 || matchState == -2); }
inline bool RebuildRoomList(bool empty, bool expired, bool lotsChanged, bool managersChanged, bool lazy) {
    return empty || expired || (!lazy && (lotsChanged || managersChanged));
}
} // namespace RoomAmbientPolicy

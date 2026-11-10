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
// An edit's room still waits (AtriumHold keeps its maps and furniture): until a solve begun after its mark ends and, for
// a room that already showed light, while it waits for its next class's solve (states 1-3). A room never solved when
// marked has nothing on screen to keep: its first solve ends the wait, even one begun before a later mark (neverSolvedNow:
// +0x100 still 4; it changes only when a solve ends), so a lot entry's re-marks do not keep a story black for 8 s.
inline bool EditRoomStillWaiting(int state, bool solvedSince, bool blankAtMark, bool neverSolvedNow) {
    return blankAtMark ? neverSolvedNow : (state >= 1 && state <= 3) || !solvedSince;
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
// A lot's loading triggers of its rooms near openings (rebuilt, options, floors or openings, dirty floor managers) are
// sent together: once none came for 600 ms (rebuilt -> dirty managers measured up to 0.52 s apart), at the latest 1.5 s
// after the first (first 0 = nothing collected)
inline bool OpeningSendDue(std::uint32_t now, std::uint32_t first, std::uint32_t latest) {
    return first && (static_cast<std::uint32_t>(now - latest) >= 600u || static_cast<std::uint32_t>(now - first) >= 1500u);
}
// The gather serial after which a room near openings is not sent again (0 = every room): a lamp edit sends every room; the
// settle skips rooms gathered after the last round ended (roundSerial, 0 = not over: every room); a collected loading send
// skips rooms gathered after its latest trigger was seen
inline std::uint32_t OpeningCoveredAfter(bool edit, bool settle, std::uint32_t roundSerial, std::uint32_t pendSerial) {
    if (edit) return 0;
    if (settle) return roundSerial ? (roundSerial > pendSerial ? roundSerial : pendSerial) : 0;
    return pendSerial;
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
// The solve's ambient step runs in state 3 only; a room marked (1) or gathered (2) keeps its last ambient until then, so a
// lamp edit's retake may still write it (a dragged lamp's rooms wait in 1-2 for the whole drag: their fill froze, 03/11)
inline bool AmbientStepRunning(int state) { return state == 3; }
// A roofed room with no lamps whose ambient was never written (both colours exactly 0: new, or solved while roofless,
// which skips the ambient step) draws its walls black: marked (1) or gathered (2) until its solve, solved (4, 5) until the
// next one. It takes what that step writes for an empty list (the unlit colour). Not 3: the step itself runs then.
inline bool UnlitSeedDue(int state, bool lamps, const float* amb, const float* amb2, const float* target) {
    const auto zero = [](const float* v) { return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f; };
    return (state == 1 || state == 2 || state == 4 || state == 5) && !lamps && zero(amb) && zero(amb2) && !zero(target);
}
inline bool AmbientUpdateDue(std::uint32_t now, std::uint32_t last) {
    return !last || static_cast<std::uint32_t>(now - last) >= 3000;
}
// Unknown or busy rooms are queued only after a change settles, never every scan.
inline bool NeedsAmbientSolve(int matchState, bool rest) { return rest && (matchState == 0 || matchState == -2); }
inline bool RebuildRoomList(bool empty, bool expired, bool lotsChanged, bool managersChanged, bool lazy) {
    return empty || expired || (!lazy && (lotsChanged || managersChanged));
}
// The instant day/night swap writes a cached solve's ambient over a room only while the room still shows what its base record
// says (rec: +0x110 and +0x120 as its solve's ambient step gave them): its second colour as recorded, and its own colour either
// as recorded or, in a room showing a stairwell's share over it, the colour its kept solve showed (shown, over the own colour
// shownOwn; null: none known)
inline bool SwapHolds(const float (*live)[4], const float (*rec)[4], const float* shown, const float* shownOwn, float tol = 1e-5f) {
    const auto near4 = [tol](const float* a, const float* b) {
        for (int k = 0; k < 4; k++)
            if (!(a[k] - b[k] <= tol && b[k] - a[k] <= tol)) return false; // (a NaN fails)
        return true;
    };
    if (!near4(live[1], rec[1])) return false;
    return near4(live[0], rec[0]) || (shown && shownOwn && near4(live[0], shown) && near4(rec[0], shownOwn));
}
// A quick day/night switch's solve window: open from the night level leaving an end (0 day, 1 night) while it reaches an end
// within kQuickMs, then kAfterMs more; a slow dusk (longer than kQuickMs at twilight) never opens it. Meanwhile the played
// lot's rooms are solved at the raised budget (kLotMs a frame for the lot pass and kDrainMs for the queue's drain: 33 ms of
// solving a frame at most) and nothing else sends them to a second solve. Fed every frame with the level (render thread).
struct DayNightWindow {
    static constexpr std::uint32_t kQuickMs = 3000, kAfterMs = 10000;
    static constexpr float kLotMs = 25.0f, kDrainMs = 8.0f;
    int end = -1;                 // the end the level was last at: 0 day, 1 night, -1 not known yet
    bool away = false, arrived = false; // between the ends since leftAt / a quick fade reached an end at arrivedAt
    std::uint32_t leftAt = 0, arrivedAt = 0;
    std::uint32_t serial = 0;     // one per window opened
    void Note(float level, std::uint32_t now) {
        const int at = level > .99f ? 1 : level < .01f ? 0 : -1;
        if (at >= 0) {
            if (away && now - leftAt <= kQuickMs) arrived = true, arrivedAt = now; // (a fade turned back counts too: its rooms are solved again)
            away = false;
            end = at;
            return;
        }
        if (end >= 0 && !away) { // the level left an end: a new window
            away = true, leftAt = now;
            arrived = false;
            ++serial;
        }
    }
    bool Open(std::uint32_t now) const {
        if (away) return now - leftAt <= kQuickMs;
        return arrived && now - arrivedAt < kAfterMs;
    }
};
} // namespace RoomAmbientPolicy

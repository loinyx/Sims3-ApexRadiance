// Offline, read-only checks of the policy used by the room updater.
#include "features/room_ambient_policy.h"
#include "features/outdoor_delta_model.h"
#include <cstdio>
#include <limits>
#include <initializer_list>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

int main() {
    using namespace RoomAmbientPolicy;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    for (float brightness : {0.0f, 0.105f, 0.344f, 0.5f, 1.0f}) {
        check(std::fabs(BackgroundShare(brightness, 1.0f) - brightness) < 1e-6f, "night matches room brightness");
        check(BackgroundShare(brightness, 0.0f) == 1.0f, "day unchanged");
        check(std::fabs(BackgroundShare(brightness, 0.5f) - (1.0f + brightness) / 2.0f) < 1e-6f, "dusk interpolation");
    }
    const float a[3] = {0.0317f, 0.0317f, 0.0317f};
    const float b[3] = {0.0169f, 0.0169f, 0.0169f};
    const float bad[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0317f, 0.0317f};
    check(SameRgb(a, a), "unchanged ambient matched");
    check(!SameRgb(a, b), "recorded stale ambient differs from target");
    check(!SameRgb(a, bad), "invalid ambient never matched");
    check(NeedsAmbientSolve(0, true), "unknown or evicted ambient queued at rest");
    check(!NeedsAmbientSolve(0, false), "unknown ambient not requeued every scan");
    check(NeedsAmbientSolve(-2, true), "busy room delegated to deferred queue");
    check(!NeedsAmbientSolve(-1, true), "outside and roofless rooms untouched");
    check(!NeedsAmbientSolve(1, true), "direct retint needs no solve");
    for (int state = 0; state <= 5; ++state)
        check(SolverOwnsAmbient(state) == (state >= 1 && state <= 3), "solver ownership");
    for (int state = 0; state <= 5; ++state)
        check(AmbientStepRunning(state) == (state == 3), "a marked or gathered room's ambient retaken");
    {
        const float zero[4] = {}, seen[4] = {0.049f, 0.049f, 0.049f, 0}, unlit[4] = {0.15f, 0.15f, 0.3f, 0};
        check(UnlitSeedDue(1, zero, zero, unlit) && UnlitSeedDue(2, zero, zero, unlit), "seed: a marked or gathered never-lit room");
        check(UnlitSeedDue(4, zero, zero, unlit) && UnlitSeedDue(5, zero, zero, unlit), "seed: a solved room left at 0");
        for (int state : {0, 3}) check(!UnlitSeedDue(state, zero, zero, unlit), "seed: not idle or while the ambient step runs");
        check(!UnlitSeedDue(2, seen, zero, unlit) && !UnlitSeedDue(2, zero, seen, unlit), "seed: not a room with an ambient");
        check(!UnlitSeedDue(2, zero, zero, zero), "seed: not a zero target");
    }
    {
        // a room floored at the empty-room colour follows the new floor; above it, it moves as MoveBackground, never under the floor
        check(std::fabs(MoveBackgroundFloored(0.049f, 0.019f, 0.030f, 0.049f, 0.080f) - 0.080f) < 1e-6f, "floored room follows the new floor");
        check(std::fabs(MoveBackgroundFloored(0.30f, 0.019f, 0.030f, 0.049f, 0.080f) - MoveBackground(0.30f, 0.019f, 0.030f)) < 1e-6f, "lit room moves as before");
        check(MoveBackgroundFloored(0.06f, 0.030f, 0.010f, 0.049f, 0.040f) >= 0.040f, "never under the new floor");
    }
    check(RebuildRoomList(false, false, false, true, false), "same lot new story manager invalidates cache");
    check(RebuildRoomList(false, false, true, false, false), "lot switch invalidates cache");
    check(!RebuildRoomList(false, false, false, false, false), "stable rooms keep cache");
    check(!RebuildRoomList(false, false, false, true, true), "lazy scan defers manager rebuild");
    check(RebuildRoomList(false, true, false, true, true), "lazy cache eventually expires");
    check(RebuildRoomList(true, false, false, false, true), "world reset rebuilds empty list");
    check(AmbientUpdateDue(100, 0), "first group update immediately eligible");
    check(!AmbientUpdateDue(1100, 1000), "second solve does not bypass cooldown");
    check(!AmbientUpdateDue(3999, 1000), "pending update waits until deadline");
    check(AmbientUpdateDue(4000, 1000), "pending update eligible at deadline without another lamp event");
    check(AmbientUpdateDue(8000, 1000), "late update remains eligible");
    check(!AmbientUpdateDue(0x20u, 0xfffffff0u), "tick wrap preserves cooldown");
    check(AmbientUpdateDue(0xc00u, 0xfffffff0u), "tick wrap eventually releases pending update");
    const float lamp = 0.0653f, oldBase = 0.02f;
    for (float base : {0.0f, 0.01f, 0.08f}) {
        const float moved = MoveBackground(lamp + oldBase, oldBase, base);
        check(std::fabs(moved - (lamp + base)) < 1e-6f, "slider preserves lamp contribution including zero");
        check(std::fabs(MoveBackground(moved, base, oldBase) - (lamp + oldBase)) < 1e-6f, "background round trip has no double addition");
    }
    float total[4] = {};
    const float first[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    const float second[4] = {0.4f, 0.5f, 0.6f, 1.0f};
    AccumulateAmbient(total, first, 2.0f, 1.0f, 2.0f);
    AccumulateAmbient(total, second, 1.0f, 1.0f, 1.0f);
    check(std::fabs(total[0] / 3.0f - 0.5f / 3.0f) < 1e-6f, "group uses original colours area and normalisation");
    check(std::fabs(total[3] / 3.0f - 1.0f) < 1e-6f, "group alpha is not normalised like RGB");
    float dark[4] = {}, black[4] = {};
    AccumulateAmbient(dark, black, 1.0f, 1.0f, 10.0f);
    check(dark[0] == 0 && dark[1] == 0 && dark[2] == 0, "unlit connected rooms reach zero");
    check(WindowRecheckDue(100, 100, 0), "window activation checked immediately after rearm");
    check(!WindowRecheckDue(2099, 100, 1), "window retry waits two seconds");
    check(WindowRecheckDue(2100, 100, 1), "late registry receives first retry");
    check(!WindowRecheckDue(6099, 100, 2), "window final retry waits six seconds");
    check(WindowRecheckDue(6100, 100, 2), "window final retry at deadline");
    check(!WindowRecheckDue(20000, 100, 3), "window checks stop after bounded passes");
    check(WindowRecheckDue(0x800u, 0xfffffff0u, 1), "window deadline survives tick wrap");
    check(!OpeningSendDue(5000, 0, 0), "nothing collected: no opening send");
    check(!OpeningSendDue(1599, 1000, 1000) && OpeningSendDue(1600, 1000, 1000), "a lone loading trigger is sent after 600 ms");
    check(!OpeningSendDue(1519, 1000, 1000) && !OpeningSendDue(2000, 1000, 1520), "dirty managers 0.52 s after rebuilt join the same send");
    check(!OpeningSendDue(1799, 1000, 1200) && OpeningSendDue(1800, 1000, 1200), "triggers in a burst wait for the latest's quiet");
    check(OpeningSendDue(2500, 1000, 2400), "a burst that never goes quiet is sent 1.5 s after its first trigger");
    check(!OpeningSendDue(0x200u, 0xfffffff0u, 0xfffffff0u) && OpeningSendDue(0x300u, 0xfffffff0u, 0xfffffff0u), "opening send survives tick wrap");
    check(OpeningCoveredAfter(true, true, 50, 40) == 0, "a lamp edit sends every room near openings");
    check(OpeningCoveredAfter(false, true, 0, 40) == 0, "a settle before its round ended sends every room");
    check(OpeningCoveredAfter(false, true, 50, 0) == 50 && OpeningCoveredAfter(false, true, 50, 60) == 60, "a settle skips rooms gathered after the round and the latest trigger");
    check(OpeningCoveredAfter(false, false, 50, 40) == 40, "a collected send skips rooms gathered after its latest trigger");
    check(LampRefreshDelay(true) == 120, "switch is reconciled after a short batch");
    check(LampRefreshDelay(false) == 200, "moving a lamp waits the drag's quiet time (the edit itself sends its rooms at once)");
    check(LampLotGap(true) == 500 && LampLotGap(false) == 2000, "relocated lamps refresh again sooner than switches");
    check(FloorPriorityFactor(false, true, 2, 2) == 1.0f && FloorPriorityFactor(false, false, 0, 2) == 1.0f, "another lot: no floor priority");
    check(FloorPriorityFactor(true, false, 2, 2) == 4000.0f && FloorPriorityFactor(true, false, 1, 2) == 2000.0f &&
              FloorPriorityFactor(true, false, 3, 2) == 1.0f,
          "one floor detailed: camera story, below, above");
    check(FloorPriorityFactor(true, true, 2, 2) == 4000.0f && FloorPriorityFactor(true, true, 0, 2) == 2000.0f &&
              FloorPriorityFactor(true, true, 5, 2) == 1000.0f,
          "every floor detailed: the camera story first, then below, then above (still before other lots)");
    check(UrgencyKept(1000, 2000, 0, 4), "urgent before its deadline");
    check(UrgencyKept(2500, 2000, 0, 3) && UrgencyKept(2500, 2000, 0, 1), "a room still waiting keeps its urgency");
    check(!UrgencyKept(2500, 2000, 0, 4) && !UrgencyKept(2500, 2000, 0, 0), "a solved room's urgency ends");
    check(!UrgencyKept(8000, 2000, 0, 3), "urgency kept at most 8 s");
    check(UrgencyKept(0x100u, 0xfffffff0u - 10u, 0xfffffff0u - 3000u, 2), "urgency survives tick wrap");
    check(!EditRoomStillWaiting(2, true, true, false), "a room blank at its mark stops waiting at its first solve (lot entry)");
    check(EditRoomStillWaiting(1, false, true, true), "a blank room waits until its first solve ends");
    check(!EditRoomStillWaiting(2, false, true, false), "a blank room re-marked mid-solve stops waiting when that solve ends");
    check(EditRoomStillWaiting(2, true, false, false), "a lit room still waits for its next class (unchanged)");
    check(!EditRoomStillWaiting(5, true, false, false), "a lit room solved since its mark stops waiting");
    check(EditHoldOutcome(0, false, 100, 100, false) == 0, "the hold waits for the first sends");
    check(EditHoldOutcome(0, false, 200, 200, false) == 1, "every story solved: released together");
    check(EditHoldOutcome(0, true, 200, 200, false) == 0, "ground chunks still written: wait");
    check(EditHoldOutcome(3, false, 2499, 2499, false) == 0, "pending rooms wait before the timeout");
    check(EditHoldOutcome(3, false, 2500, 2500, false) == 2, "timeout 2.5 s after the latest edit");
    check(EditHoldOutcome(3, false, 600, 4000, false) == 2, "timeout 4 s after the first edit");
    check(EditHoldOutcome(0, false, 5000, 9000, true) == 0, "never released during a drag");
    check(EditHoldOutcomeTogether(1, false, 2600, 4100, false, 6000) == 0, "all floors together: a pending room waits past the old 2.5 s / 4 s bounds");
    check(EditHoldOutcomeTogether(0, false, 300, 300, false, 6000) == 1, "all floors together: released once every room is solved");
    check(EditHoldOutcomeTogether(2, false, 900, 6000, false, 6000) == 2, "all floors together: bounded timeout");
    check(EditHoldOutcomeTogether(0, false, 9000, 9000, true, 6000) == 0, "all floors together: never during a drag");
    check(EditHoldOutcomeTogether(0, false, 100, 100, false, 6000) == 0, "all floors together: the first marks come first");
    check(GatherAfterChange(1001, 1000), "gather started after final switch can be retained");
    check(!GatherAfterChange(999, 1000), "gather started before switch cannot be retained");
    check(!GatherAfterChange(1000, 1000), "same millisecond is ambiguous and requeues");
    check(GatherAfterChange(0x20u, 0xfffffff0u), "fresh gather comparison handles tick wrap");
    check(!GatherAfterChange(0xfffffff0u, 0x20u), "old gather before tick wrap cannot be retained");
    for (int state = -1; state <= 6; ++state)
        check(RetainFreshSolve(state) == (state == 2 || state == 3 || state == 5), "retain only post-gather solve states");
    check(AmbientMapsCompatible(1, 1, 42.85f, 42.85f), "colour-only update preserves map scale and wall ramp");
    check(!AmbientMapsCompatible(1, 0.5f, 42.85f, 42.85f), "normalisation change must solve again");
    check(!AmbientMapsCompatible(1, 1, 42.85f, 46.67f), "wall ramp change must solve again");
    check(!AmbientMapsCompatible(0, 0, 42.85f, 42.85f), "invalid zero normalisation falls back");
    check(!AmbientMapsCompatible(1, std::numeric_limits<float>::infinity(), 42.85f, 42.85f), "infinite normalisation falls back");
    check(!AmbientMapsCompatible(1, 1, std::numeric_limits<float>::quiet_NaN(), 42.85f), "invalid wall base falls back");
    check(!RigFallbackDue(2499, 1000, false), "busy lot waits before fallback");
    check(RigFallbackDue(2500, 1000, false), "busy lot keeps original furniture fallback");
    check(!RigFallbackDue(3000, 1000, true), "fallback does not repeat while waiting");
    check(RigFallbackDue(0x700u, 0xfffffff0u, false), "furniture fallback handles tick wrap");
    // Event-order model: a fresh solve may finish before or after the 120 ms reconciliation.
    // The production retention rule must not restart either ordering, including a second switch.
    for (const std::array<int, 4> states : {std::array<int, 4>{3, 2, 5, 1}, {5, 3, 2, 1}}) {
        int restarts = 0;
        for (int state : states) restarts += !RetainFreshSolve(state);
        check(restarts == 1, "only ungathered member requeues in either completion order");
        check(!GatherAfterChange(1001, 1100), "second switch invalidates retained first-switch gather");
    }
    // The native group still progresses with cached sources; no all-members-ready gate.
    for (float lampColour : {0.0f, 0.12f}) {
        const float source[4] = {0.0287f + lampColour, 0.0287f, 0.0287f, 1};
        float merged[4] = {};
        for (int room = 0; room < 4; ++room) AccumulateAmbient(merged, source, 1, 1, room + 1.0f);
        check(std::fabs(merged[0] / 10 - source[0]) < 1e-6f, "same map-scale group converges when on or off");
    }
    // A quick day/night switch's solve window (DayNightWindow): opens as the level leaves an end, stays open kAfterMs after a
    // quick arrival, never opens for a slow dusk, opens again at the next switch (a new serial)
    {
        DayNightWindow w;
        w.Note(1.0f, 1000);
        check(!w.Open(1000) && w.serial == 0, "at night with no switch the window is closed");
        w.Note(0.96f, 2000);
        check(w.Open(2000) && w.serial == 1, "the level leaving night opens the window");
        w.Note(0.0f, 2600);
        check(w.Open(2600) && w.Open(12599) && !w.Open(12601), "a quick arrival keeps it open 10 s");
        w.Note(0.4f, 20000);
        check(w.Open(20000) && w.serial == 2, "the next departure opens a new window");
        w.Note(0.5f, 23001);
        check(!w.Open(23001), "a slow dusk (over 3 s between the ends) closes it");
        w.Note(1.0f, 60000);
        check(!w.Open(60000), "a slow dusk's arrival opens nothing");
        w.Note(0.9f, 70000);
        w.Note(1.0f, 70500);
        check(w.Open(70500) && w.Open(80499), "a fade turned back counts as a switch");
        DayNightWindow load;
        load.Note(0.5f, 100);
        check(!load.Open(100) && load.serial == 0, "a world loaded at twilight opens nothing until it reaches an end");
        check(DayNightWindow::kLotMs + DayNightWindow::kDrainMs <= 33.0f, "at most 33 ms of solving a frame in the window");
    }
    // Replay of the first switch of a day/night recording (lot CF2DEA20, departure at 0): the played lot's solves in the
    // game's order with the game's solve time of each (ms, its per-class total read every ~0.2 s). Before (measured): every
    // repeat as it was (story 1 room 2 three times, room 3 twice, room 8 three times plus one thrown away) at the budget the
    // steady pacing left, last end +15.53 s. After: the normalisation and retint resends gone (each room once, plus the repeats
    // the game's window marks still cause: story 2 room 7 twice), at the window's 25 + 8 ms a frame over a 14 ms frame (73 fps
    // before the switch).
    {
        struct Solve { const char* room; float ms; };
        const Solve after[] = {{"s1r4", 37}, {"s3r10", 13}, {"s0r13", 28}, {"s2r7", 40}, {"s2r7", 80}, {"s1r1", 80}, {"s1r2", 668},
                               {"s1r3", 119}, {"s2r12", 40}, {"s1r8", 100}, {"s2r5", 189}, {"s2r6", 100}};
        const float frameMs = 14.0f, budget = DayNightWindow::kLotMs + DayNightWindow::kDrainMs;
        float t = 0.0f, left = 0.0f, worst = 0.0f, bigEnd = 0.0f;
        size_t next = 0;
        while (next < std::size(after) || left > 0.0f) {
            float spent = 0.0f;
            while (spent < budget && (left > 0.0f || next < std::size(after))) {
                if (left <= 0.0f) left = after[next++].ms;
                const float step = std::min(left, budget - spent);
                left -= step, spent += step;
                if (left <= 0.0f && std::strcmp(after[next - 1].room, "s1r2") == 0) bigEnd = t + frameMs + spent;
            }
            worst = std::max(worst, spent);
            t += frameMs + spent;
        }
        std::printf("replay first switch: last room +%.2f s (measured before: +15.53 s), story 1 room 2 +%.2f s (before: +10.01 s, "
                    "solved 3 times), at most %.0f ms of solving a frame\n", t / 1000.0f, bigEnd / 1000.0f, worst);
        check(t < 3000.0f, "the replayed switch ends within 3 s");
        check(worst <= 33.0f, "no replayed frame solves more than 33 ms");
    }
    // The day/night swap's ambient (SwapHolds): a stairwell room shows its own colour plus its neighbours' share, so the swap
    // accepts the colour its kept solve showed over that solve's own colour, and nothing else
    {
        const float rec[2][4] = {{.20f, .21f, .22f, 1.0f}, {.10f, .11f, .12f, 1.0f}};
        const float shared[4] = {.23f, .24f, .25f, 1.0f}, other[4] = {.30f, .30f, .30f, 1.0f};
        float live[2][4];
        std::memcpy(live, rec, sizeof rec);
        check(SwapHolds(live, rec, nullptr, nullptr), "swap: a room holding its solve's pair is written");
        std::memcpy(live[0], shared, sizeof shared);
        check(!SwapHolds(live, rec, nullptr, nullptr), "swap: another own colour with no kept share is refused");
        check(SwapHolds(live, rec, shared, rec[0]), "swap: a stairwell's share over its solve's own colour is written");
        check(!SwapHolds(live, rec, shared, other), "swap: the share over another own colour than the record's is refused");
        check(!SwapHolds(live, rec, other, rec[0]), "swap: a colour that is not the kept solve's share is refused");
        live[1][0] += .01f;
        check(!SwapHolds(live, rec, shared, rec[0]), "swap: a second colour not the record's is refused");
        live[1][0] = NAN;
        check(!SwapHolds(live, rec, shared, rec[0]), "swap: a NaN is refused");
    }
    // A kept atrium wall relit without a solve (FollowGhosts, GhostKeptPS): the rows lit beyond its edges follow the change of
    // their edge texel, so the blur across the floor line does not pull the relit edge back to the solve's light
    {
        using OutdoorDeltaModel::FollowGhosts;
        const uint32_t unlit = 0x10202020u, lit = 0x10A0B0C0u, ghostDark = 0x10181818u;
        // ext: 2 rows above, 2 block rows, 2 rows below, 2 columns
        uint32_t ext[12] = {ghostDark, ghostDark, ghostDark, ghostDark, lit, unlit, unlit, lit, ghostDark, ghostDark, ghostDark, ghostDark};
        const uint32_t solved[4] = {unlit, unlit, unlit, unlit};
        uint32_t same[12], sameBefore[12];
        std::memcpy(same, ext, sizeof ext);
        same[4] = unlit, same[7] = unlit;
        std::memcpy(sameBefore, same, sizeof same);
        FollowGhosts(same, solved, 2, 2, true, true);
        check(std::equal(std::begin(same), std::end(same), std::begin(sameBefore)), "ghosts: an unchanged relight leaves the rows beyond as the solve lit them");
        FollowGhosts(ext, solved, 2, 2, true, true);
        check(ext[0] == 0x1098A8B8u && ext[2] == 0x1098A8B8u, "ghosts: the rows above follow their relit edge texel");
        check(ext[1] == ghostDark && ext[3] == ghostDark, "ghosts: a column whose edge kept its light keeps its rows above");
        check(ext[9] == 0x1098A8B8u && ext[11] == 0x1098A8B8u && ext[8] == ghostDark, "ghosts: the rows below follow the bottom edge");
        uint32_t clampHi[3] = {0xF0F0F0F0u, 0xF0F0F0F0u, 0xFF000000u};
        const uint32_t from[1] = {0x00000000u};
        FollowGhosts(clampHi, from, 1, 1, true, false);
        check(clampHi[0] == 0xFFF0F0F0u && clampHi[1] == 0xFFF0F0F0u, "ghosts: a byte past 255 is clamped");
        uint32_t clampLo[3] = {0x00000000u, 0x00000005u, 0x00000005u};
        const uint32_t fromLit[1] = {0x00000020u};
        FollowGhosts(clampLo, fromLit, 1, 1, false, true);
        check(clampLo[1] == 0u && clampLo[2] == 0u, "ghosts: a byte under 0 is clamped");
        uint32_t none[2] = {lit, unlit};
        FollowGhosts(none, solved, 2, 1, false, false);
        check(none[0] == lit && none[1] == unlit, "ghosts: a block with no rows beyond is left alone");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

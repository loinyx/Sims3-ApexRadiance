# Lot Lighting While Moving: validation

The feature is described in [features/performance/lot-lighting-motion.md](../features/performance/lot-lighting-motion.md).
It must:

- Scale the lot lighting budget while the camera moved in the last 300 ms, and with the camera still only by the
  frame-time pacing (never while a lamp edit's rooms wait, in the settle window or during a lamp switch's boost); only
  budgets under 100 ms.
- Return `max(floor, min(game, game x budgetMs / 15))` while moving (floor 0.25 ms, and 3/15 of the game's 30 / 10 ms
  budget for a loading lot), and the game's value with the camera still while the frames hold.
- Read the camera eye without faulting when no world is loaded.

## Automated tests

`tools/room_solve_share_test` (in the Smoke suite of `tools/development/check.ps1`) checks the shared room solve time's
decision and replays the lot lighting update's loop (levels from level 0, at least one per lot, each lot's budget) over a
synthetic pan with and without the gate: no level waits more than 30 frames, the worst moving frame is not longer, and
once the camera stops every level ends in the same state as without the gate. The budget hook itself has no offline
harness.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-22 | `0b6b5af` | None: no offline harness covers the budget hook or the steady pacing | Not measured | not run in game yet |
| 2026-11-11 | branch `release/3.1.2` | None: no offline harness covers the loading floor | Not measured | not run in game yet |
| 2026-11-17 | branch `feature/lot-room-solve-budget` | `tools/room_solve_share_test` (5 seeds, 9 lots x 3 levels, 120 moving frames; synthetic) | Moving solve time per frame mean 11.8-13.6 -> 5.9-6.3 ms, worst 29.9-62.1 -> 21.3-28.8 ms; longest wait 22 frames; every level ends as without the gate, 3 to 25 frames later | not run in game yet |

## In-game test plan

1. **Expected log:** `[LotLightingMotion] On: the call at 0x00adb95d ...` with the camera offsets (root 0x011d1860,
   +0x24, +0x60 on Steam).
2. Developer card. **Expected:** "Camera moving" while panning, "Camera still" 0.3 s after stopping; "last budget: game
   15.00 ms -> 3.00 ms" while moving over the current lot, "game 5.00 -> 1.00" for other lots.
3. Frame Profiler, camera test. **Expected:** the 16 to 50 ms moving hitches dominated by "Lot room solve" drop; "Lot
   room solve" per hitch about 3 ms instead of about 15.
4. Visual: after stopping, the current lot's lights and a newly loaded lot's lights settle within about a second. Try 1,
   3 and 6 ms. At night with Night Lighting, lamps placed or removed still relight their rooms (a moment later while the
   camera moves).
5. Build / Buy mode and Edit Town still relight at once when the camera is still.

Shared room solve time (`sharedSolveTime = true`):

6. Frame Profiler timing-run preset on a pan over a town's lots on any night save (the `room-solve-share-pan` runner job), with the option off
   and on. **Expected:** with it on, fewer moving hitches led by "Lot room solve", lower "Lot room solve" ms per hitch
   frame, `room_solve_waited` above 0 only while `apex_moving` is 1, `room_solve_longest_wait` never above 30, and the
   developer card's "longest wait" at most 30 frames.
7. Stop the camera after the pan. **Expected:** the timeline's `room_solve_waited` is 0 from about 0.3 s after the stop,
   and the lots' lights match a run with the option off (same scene, same time of day) once both have settled.

## Confirmed in game

- Nothing recorded beyond its release.

## Open checks

- 2.15.1 settle pacing: frame times while lots load, against 2.15.0 on the same save, not yet measured.
- Why the current lot relights continuously while the camera moves.
- Paced relight while moving (`715a410`): pan right after a load and right after switching lamps, with the Tester switch
  *Paced relight while moving* off and on. **Expected:** fewer hitches while moving with it on (the developer card's
  *settle or lamp switch paced* count grows), the same final light, and on a fast PC relighting about as quick as before.
  Not yet measured in game.
- Paced relight at rest (`0b6b5af`): camera still at night on a busy lot while rooms relight (dusk, Night Lighting's
  *relight lots*, a lot after its settle window), with the Tester switch *Paced relight at rest* off and on, in the
  Frame Profiler. **Expected:** with it on, fewer of the 25 to 50 ms frames dominated by "Lot room solve"; the developer
  card's *Steady pacing* below 100% only while frames run long, with its *lowered in N budget calls* count growing; the
  same final light. Lamps moved, dragged or switched in Build mode relight as quickly as with it off. Not yet measured in
  game.
- 3/15 loading floor (3.1.2): not run in game. At 1 ms, pan over lots that load. **Expected:** the developer card shows
  "game 30.00 ms -> 6.00 ms" or "game 10.00 ms -> 2.00 ms" while a lot loads, and lots requested while panning finish
  without waiting for the camera to stop.

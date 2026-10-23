# Lot Lighting While Moving: validation

The feature is described in [features/performance/lot-lighting-motion.md](../features/performance/lot-lighting-motion.md).
It must:

- Scale the lot lighting budget while the camera moved in the last 300 ms, and with the camera still only by the
  frame-time pacing (never while a lamp edit's rooms wait, in the settle window or during a lamp switch's boost); only
  budgets under 100 ms.
- Return `max(0.25, min(game, game x budgetMs / 15))` while moving, and the game's value with the camera still while
  the frames hold.
- Read the camera eye without faulting when no world is loaded.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-22 | `0b6b5af` | None: no offline harness covers the budget hook or the steady pacing | Not measured | not run in game yet |

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

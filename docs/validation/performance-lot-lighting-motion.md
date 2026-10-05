# Lot Lighting While Moving: validation

The feature is described in [features/performance/lot-lighting-motion.md](../features/performance/lot-lighting-motion.md).
It must:

- Scale the lot lighting budget only while the camera moved in the last 300 ms, and only for budgets under 100 ms.
- Return `max(0.25, min(game, game x budgetMs / 15))` while moving, the game's value otherwise.
- Read the camera eye without faulting when no world is loaded.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No recorded measurement | |

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

- Why the current lot relights continuously while the camera moves.

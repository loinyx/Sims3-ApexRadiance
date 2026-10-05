# Wall Shading While Moving

While the camera moves, the soft ambient shading of the outdoor walls of newly loaded lots waits until the camera stops
(or two seconds pass), and never more than one wall shading pass runs per frame. Panning over a neighbourhood that is
loading stutters less. The shading detail itself is unchanged; walls of a lot loaded while panning may look flat for a
moment and get their shading within a few frames of stopping.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (on by default since its introduction) |
| Default | On |
| Menu | System > Performance > Camera and lighting > *Wall shading waits while moving* |
| Configuration | `[patches.WallShadingWhileMoving]` in `ApexRadiance.toml` |
| Source | [`features/lot_lighting_motion.{h,cpp}`](../../../features/lot_lighting_motion.cpp) (`StartWallAo`, `Hook_WallAoStep`), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

Each lot level has a wall ambient-occlusion (AO) solver. Its step shades every outdoor wall of the level in one pass and
never looks at the frame budget it is given. With *Spread lot lighting while moving* on, every remaining moving "Lot
room solve" hitch of 5 ms or more is one such pass: 10 to 17 ms each, and 54 to 108 ms when lots load. A lot finishes
loading only after the first pass of all its levels, so the pass cannot simply be skipped.

## How Apex Radiance solves it

The solver step's vtable slot goes through a gate. For calls from the solver driver with a frame budget, in state 0
(first pass) or 1 (refinement):

1. **While the camera moves,** return the current state without a pass. This is exactly what the engine does itself in
   state 1 when its cost estimate is negative: the driver stores the state unchanged and asks again next frame.
2. **Bound the wait:** one wait per state and per camera motion, at most 2 s. After that, the pending passes of that
   state run for the rest of the motion, one per frame.
3. **Always,** run at most one pass of 1 ms or more per frame across all lots.
4. **When the camera stops,** clear the waits; the waiting passes run one per frame.

Every other call (the synchronous level solve, the tool mode, any unknown caller or state) goes to the game unchanged.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Wall shading waits while moving | `[patches.WallShadingWhileMoving] enabled` | bool | on | | Turns the gate on. Independent of *Spread lot lighting while moving*, whose camera detection it shares |
| Developer > Performance > *Longest wait of a pass while moving (ms)* | `[developer.controls.lot_lighting_motion] wall_max_wait_ms` | int | 2000 | 0 to 10000 | The longest wait per state and camera motion (`firstPassWaitMs`) |

## Compatibility and interactions

- **Spread lot lighting while moving:** independent feature sharing the camera detection and its per-frame Present
  sample.
- **Frame Profiler:** its "Wall AO pass" counter is the inner layer, so it times only the passes that run; "Lot room
  solve" includes them (they happen inside it). The report adds "Wall shading while moving: ...".
- **Night Lighting:** never resets the wall AO (only message 0x0486519D and level creation do); its room relights are
  unaffected.
- **Official Sims3SettingsSetter:** its source has none of the AO step, driver or slot; its lighting quality patch hooks
  0x006A0E00 / 0x006A4480 / 0x0069FD60 / 0x006A8D20 / 0x006A8C88 / 0x006A8DE0, none of which is the AO solver.

## Limitations

- A lot that loads while you pan finishes loading (is shown instead of its impostor) up to about 2 s later.
- Outdoor walls of such a lot get their AO shading when the camera stops (or after 2 s), one level per frame.
- Build mode, Edit Town (tool mode) and lot LOD switches are passed through and behave as without the gate.

## Technical reference

### The game side (Steam 1.67.2)

- Every lot level embeds two solvers, `lvl+0x290` (wall AO, vtable 0x00FF0594) and `lvl+0x2E8` (vtable 0x00FF0714).
  0x006A8BA0 (the room solve, from the lot lighting update) calls slot +0xC of both when `[lvl+0x88] >= 0`.
- **0x00688920, the solver driver** (slot +0xC of both vtables), thiscall(stopwatch*, float budget), ret 8:
  `if ([s+0x14] != 2 && [s+8]->vfunc+0xC()) [s+0x14] = s->vfunc+0x1C(stopwatch, budget)`. The step's return value
  becomes the state; the driver asks again every frame until it is 2. The CALL is `FF D2` at +0x2B; its return address
  (+0x2D) is `89 46 14` (the store).
- **0x0068B810, the wall AO step** (slot +0x1C of 0x00FF0594 = 0x00FF05B0, its only reference), thiscall, ret 8,
  returns the next state:
  - level `[s+4]`, outdoor room = RoomById(level, 0x005BFB90()) (0x006A6550). No room: returns `[level+0x280] ? 2 : 0`.
    No walls (`(room+0xDC - room+0xD8) / 4 == 0`): `[s+0x18] = 0`, returns 2.
  - **state 1 (refinement):** `v = [s+8]->vfunc+0x10()`; v < 0 returns 1 (the engine's own "not now, try again next
    frame"); else picks the highest detail level `i < MaximumDetailLevel` whose predicted cost stays under AO.ini
    WallMillisecondsBudget (`[0x011CF4A0+0x24]`, int ms, 10); none: 2; else one pass at that level, returns 2.
  - **other states (0, first pass):** one pass at detail 0, `[s+0x18]` = its elapsed ms, returns 1.
  - a pass: lock the AO image (0x00618DF0), 0x0068B2B0 for every wall, unlock (0x00619160). The stopwatch is read only
    after the loop; the budget argument is not used at all. The AO ray loop at 0x0068AF31 dominates its time.
- **Who waits for it:**
  - 0x00688DB0 binds the AO image to the walls only when the state is 1 or 2 (before the first pass: walls without AO).
  - **Lot load stage 20** (case 20 of the jump table 0x00AEB280 in the lot load state machine 0x00AEA680) calls
    0x00ADBBA0 -> 0x006A5B50 for every level: both solvers' state != 0. If not, the stage yields and retries next frame.
    Then it clears the lot lighting "loading" flag `[+0x4F]` (budgets 10/30 -> 5/15), and stage 21 sets the lot
    renderer's "loaded" flag `[+0x1E]`, which the lot's render managers (0x00AE4D80) and the impostor LOD switch
    0x00AD9E30 (returns "retry" (7) while not loaded) wait for. So a lot finishes loading only after the first pass of
    all its levels.
  - 0x006A5BF0 (both states == 2) <- 0x00ADBC30 <- 0x00AE06B0 (a jump thunk from the lot renderer, +0x240) <- the
    ThumbnailManager's lot capture (CALL 0x00D5BE2F, "UI/ThumbnailManager", state machine near 0x00D5A2E0): a lot
    thumbnail returns "not yet" until every level's refinement ran. Nothing else waits for the refinement.
- Resets to state 0: slot +0x10 0x006895C0 (from message 0x0486519D through slot +4, and 0x006A4240 / 0x006A4180) and
  level creation.
- **Other callers of the driver:** the synchronous level solve 0x006A4180 (from the lot LOD switch setup 0x00ADBAD0 <-
  0x00AEB3F0 <- 0x00AD9E30) resets both solvers and drives each once with the 60,000 ms budget `[0x00FF3460]`. The tool
  mode passes 1000 ms. The impostor pump 0x00AD97E0 runs the lot pass (0x00C7CEA0) only in the tool mode.

### The gate

- Slot swap of 0x00FF05B0 through `SlotChain` (site `WallAoStep`, layer `Gate`, the outermost: the gate recognises the
  driver by its own return address, so nothing may sit outside it). No code bytes change.
- Start checks the whole driver body (52 bytes) and the step's first 69 bytes (`kStepBytes`) against the studied code;
  any difference leaves the feature off with a message.
- The gate acts only when its return address is the driver's (+0x2D), `0 < budget < 100 ms`, and the state is 0 or 1.
- **While the camera moves** (shared camera signal, 300 ms hold): state 0 returns 0 and state 1 returns 1 without a
  pass. The wait starts at the first deferral of that state since the camera was last seen still and lasts at most
  `firstPassWaitMs`. Once it ran out, pending passes of that state run for the rest of the motion, one per frame; the
  wait is not restarted by those passes. `OnPresentSample` clears both waits at every Present without motion; a pass
  that runs while still clears its own.
- **Per-frame limit:** a frame is the time between two Presents, counted by the Present callback; when no Present came
  for 250 ms, a frame is 33 ms. Passes that take under 1 ms (no room, no walls) do not use up the frame.
- Nothing else reads the step's return value.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| WallAoStep / WallAoStepSlot | 0x0068B810 / 0x00FF05B0 | Sig (entry; alternate at +0x21) / SlotsOf(1) |
| WallAoDriver | 0x00688920 | Sig (the whole body; alternate at +0x1B) |

Group `WallShadingWhileMoving`.

### Source

`features/lot_lighting_motion.cpp`: `StartWallAo` / `StopWallAo`, `Hook_WallAoStep`, `FramePassUsed`, `RunPass`,
`WallAoStatusText`, `RenderWallAoDeveloperUI`.

Developer card *Wall shading*: step calls, first passes / refinements run, held while moving (first / refinement),
passes run after the wait, moved to a later frame, passed through, pass time (average, longest, last).

### Rules for maintainers

- The gate must stay the outermost layer of 0x00FF05B0. A layer outside it would make every call "another caller"
  (passed through: safe, but the gate would do nothing).
- Do not defer a pass without a bound: lot load stage 20 waits for the first pass (the lot would stay on its impostor for
  as long as the camera moves) and the ThumbnailManager's lot capture waits for the refinement.
- Do not defer calls with a budget of 100 ms or more: the synchronous level solve (60 s) and the tool mode (1000 ms,
  whose impostor pump runs the lot pass without Presents) expect the pass to run.

## Rejected approaches

- Restarting the wait after each pass that ran out its wait: made every other lot start a fresh wait.
- Capping WallMillisecondsBudget while moving: only shortens the refinement and would change the detail chosen for good.
- Making the pass resumable per wall: a re-implementation, not needed unless the deferral looks bad.

Details in [history](../../history/performance-wall-shading-while-moving.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-wall-shading-while-moving.md)
- [History](../../history/performance-wall-shading-while-moving.md)
- [Room light maps](../../engine/room-light-maps.md)

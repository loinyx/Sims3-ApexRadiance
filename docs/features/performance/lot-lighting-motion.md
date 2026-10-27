# Lot Lighting While Moving

While the camera moves, lots relight in small steps each frame instead of taking up to 15 ms at once, so panning over
busy neighbourhoods stutters less. With the camera still, lots relight at the game's own pace while the frame rate
holds, and in smaller steps when frames run long. Nothing is skipped: the room solves resume on the next frame, and
every room still gets the game's complete solve.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (on by default since its introduction). Lot build slice while moving, the lamp edit and after-load budgets and the refinement cap: Released in 2.7.0. A lamp edit's room while moving: Released in 2.12.3. Spread cap after a drag: Released in 2.13.0. Settle window after entering a lot: Released in 2.15.0. Settle window paced by the frame time: Released in 2.15.1. Paced budgets while moving and steady pacing with the camera still: Released in 2.15.3. Shared lot build time: Released in 2.16.2 |
| Default | On, 3 ms |
| Menu | System > Performance > Camera and lighting > *Spread lot lighting while moving* |
| Configuration | `[patches.LotLightingMotion]` in `ApexRadiance.toml` |
| Source | [`features/lot_lighting_motion.{h,cpp}`](../../../features/lot_lighting_motion.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

Every frame, the lot lighting update solves dirty rooms of each lot within a time budget. The current ("priority") lot
gets 15 ms per frame, and that budget is usually spent in one frame while the camera moves, which shows up as 16 to 50
ms hitches dominated by "Lot room solve" in the Frame Profiler. Why the current lot keeps relighting while the camera
moves is not established (see [validation](../../validation/performance-lot-lighting-motion.md)); Night Lighting
also queues room relights.

## How Apex Radiance solves it

The single CALL that fetches the budget is redirected to Apex Radiance:

1. Call the game's budget function and read its result.
2. If the camera moved in the last 300 ms and the budget is under 100 ms, return
   `max(0.25, min(game, game x budgetMs / 15))`.
3. Otherwise return the game's value.

At the default 3 ms the priority lot goes from 15 to 3 ms (30 to 6 while loading) and other lots from 5 to 1 ms (10 to 2
while loading). The engine's own order and the priority lot's larger share stay; only the time per frame shrinks. The
tool mode's 1000 ms budget is never changed.

The same hook also adjusts the budget around lamp edits and loads (budgets of 15 ms or more and under 100 ms, that is
the lot being played):

- **A lamp just switched:** the lot keeps a raised budget for a moment (`Boost`): the game's own with the camera still,
  and the paced budget below while it moves.
- **After a load or entering a lot:** for 15 s after the world goes live, and for 10 s after the camera enters a lot
  (`LevelLightShare::SettlingAfterLoad`), the lot being played gets up to 25 ms with the camera still, so a lot corrects
  its lighting in one round, and the paced budget below while it moves. With the camera still the raise (and the room
  queue's drain raise from 4 to 12 ms, [Faster room lighting](room-light-queue.md)) is dosed by the frame time
  (`PaceSettle`, on the Present sample): the baseline is the frame time outside the window; in it a frame over
  max(16.7 ms, 1.5 x baseline) takes 0.15 off the scale, a frame under 0.8 x that target gives 0.05 back (each window
  starts at 0.5). The game budget becomes `game + (25 - game) x scale`, the drain `4 + (12 - 4) x scale`. A fast PC
  keeps the quick light, a PC at its limit keeps the game's own budget (an F8 session while lots loaded: frames of 31 to
  37 ms of room solving in 276 of 440 hitches). Without the Present sample (both camera-motion options off) the scale
  stays 1. Developer status: *Settle pacing* on the Lighting while the camera moves card.
- **Paced while moving:** during a lamp switch's boost or the settle window, a moving camera gives
  `moving + (game - moving) x scale`, where *moving* is the scaled budget above and *scale* the same frame-time pacing
  (a lamp switch's boost is a pacing window too): the game's budget while the frames hold, down to the moving budget
  when they do not. These two raises caused many of the hitches while panning (a frame-time recording: 61 of 197). The
  lamp edit room's 6 ms below still applies. Only how fast those rooms relight while panning changes, not their light.
  Tester switch *Paced relight while moving* (`perf_motion_paced_raise`, on); off, both keep the game's full budget
  while moving.
- **A lamp edit's room while moving:** while the room being solved is an edit's urgent room
  (`RoomLightQueue::CurrentRoomUrgent`), the lot being played gets the game's budget up to 6 ms (`kEditMovingMs`)
  instead of the scaled one, so an edit made while panning does not wait for 3 ms slices. Which lot a budget call serves
  is not read, so a neighbouring lot's pass is not raised.
- **After a drag stops:** while the rooms that waited for a dragged lamp to stop are solved (at most 15 s), the frame's
  room solves stay under the spread cap of 3.5 ms (`DragLite::SolveCapMs`): the lot being played gets what *Faster room
  lighting*'s drain left of it, the other lots 0.25 ms, with no edit boost. While the lamp's own rooms still wait there
  is no cap, and the other lots keep 0.25 ms ([lamp drags](../night-lighting/lamp-drags.md)). Tester switch *Spread the
  update after the stop* (`drag_spread_cap`, on).
- **A lamp edited in Build mode:** while its rooms wait for or are in their solve, the lot being played gets 25 ms with
  the camera still once the lamp is let go; while it is dragged the game's 15 ms stays.
- **Refinement after a quick pass:** while the rooms of a lamp switch that already show their quick pass are refined
  ([Faster room lighting](room-light-queue.md), *Quick update for lamp switches*), the budget is capped at 6 ms so the
  frame rate holds.

**Camera still: steady pacing.** With the camera still, the game's own budgets (15 ms for the lot being played, 5 ms for
the others; not the loading budgets of 30 and 10 ms, which a lot's load waits on) are dosed by the frame time:
`moving + (game - moving) x scale`, where *moving* is the budget the camera motion rule above would give. Hitches with
the camera still were mostly room solves: the game's 15 ms plus the room queue's drain made 25 to 50 ms frames. The
scale (`g_steadyScale`, `PaceSettle`, every Present sample) starts at 1; its baseline follows the lower envelope of the
frame time (20% of the way down towards a shorter frame, 0.5% of the way up towards a longer one, so a run of solving
frames does not raise it); a frame over max(16.7 ms, 1.5 x baseline) takes 0.15 off, a frame under 0.8 x that target
gives 0.05 back. While the frames hold the lot keeps the game's budget; when they run long it goes down towards the
moving budget. The room queue's drain takes the same scale: `1 + (4 - 1) x scale` ms instead of 4
([Faster room lighting](room-light-queue.md)). It does not apply while a lamp edit's rooms wait
(`LevelLightShare::LampEditPending`, `RoomLightQueue::CurrentRoomUrgent`), in the settle window after a load or during a
lamp switch's boost: those keep their own budgets. Without the Present sample (both camera-motion options off) the scale
stays 1. Only how soon rooms finish changes, not their light. Tester switch *Paced relight at rest*
(`perf_steady_room_pacing`, on); off, the game's budget every frame.

**Lot build slice.** A streaming lot is built a slice per frame by 0x00AEA680 with a 20 ms budget (35 ms for a priority
lot). While the camera moves Apex lowers that slice to 6 ms; the loading screen's 2000 ms is never changed. The feature
also starts the [shared lot build time](lot-streaming.md#shared-lot-build-time), which first caps each slice to what the
frame's shared budget leaves.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Spread lot lighting while moving | `[patches.LotLightingMotion] enabled` | bool | on | | Turns the budget scaling on |
| Lot lighting time while moving (shown while the switch is on) | `[patches.LotLightingMotion] budgetWhileMovingMs` | int | 3 | 1 to 15 ms | The current lot's time per frame while moving. End labels "Smoother" / "Lights sooner"; 15 = the game's own. Applied at once (the hook reads it on every call; `Update` clears the reinstall request). The key is never renamed |

## Compatibility and interactions

- **Night Lighting:** its room relights (QueueRoom 0x006C7160 on the light tree levels: at world load, dusk, the
  "relight lots" button, lamp edits) and level light share's outdoor re-gathers are solved by this same budgeted
  update. While the camera moves they take more frames (the rooms resume where they stopped, as the game already does at
  its own budgets). Nothing in Night Lighting waits for a relight to finish within a frame.
- **Wall shading waits while moving:** independent; it shares this page's camera detection and per-frame Present
  sample. The budget cap makes the room solves resumable pieces; the wall gate removes the one piece that is not.
- **Spread new objects over frames** and **Faster room lighting** use the same camera sampler.
- **Frame Profiler:** keeps both its lot lighting targets (the entry of 0x00ADB8F0 with Detours; the CALL 0x00ADB9AD).
  Its "Lot room solve" calls receive the scaled budget; one call is one lot level. The report adds "Lot lighting while
  moving: ...".
- **Official Sims3SettingsSetter:** its patterns overlap neither the budget call 0x00ADB95D / 0x00ADB120, the room solve
  0x006A8BA0 nor the camera read 0x00C6D5BD (its LSO detours WorldManager::Update at 0x00C6D570; the camera site is
  0x4D bytes later and only read).
- The removed Smooth Streaming feature detoured 0x00ADB120's entry, which the call-site redirect would pass through
  ([removed features](../../removed-features.md)).

## Limitations

- The current lot's lights, and lots streaming in, finish over more frames while the camera moves. They catch up within
  about 300 ms of stopping plus the remaining work at full budget.
- With the camera still on a PC whose frames run long, rooms relit by something other than a lamp edit (dusk, a load
  still solving after its settle window, Night Lighting's requeues) finish over more frames than with the game's budget.
- Budgets of 100 ms or more (tool mode) are never scaled.

## Technical reference

### The game side (Steam 1.67.2)

- **0x00ADB8F0** lot lighting update, thiscall(lot lighting manager), from the lot renderer update 0x00AEB2E0 ->
  0x00AE4CB0 (render thread, per lot with lighting work, every frame; also reached from the lot impostor builder
  0x00AD9E30 -> 0x00AEB3F0). Returns at once unless `[this+0x18]` and not `[this+0x4E]`. Starts an EA stopwatch in ms
  (0x004F35B0(4, 0), 0x00408700), then `call 0x00ADB120` at 0x00ADB95D; `fst [esp+0Ch]` keeps the budget for the loop
  test while ST0 stays loaded. For each level object of the lot (deque at `this+0x24..0x40`): 0x006A8BA0(stopwatch,
  budget) (the CALL at 0x00ADB9AD), then `elapsed = 0x004F33C0`; stop when elapsed > budget. At least one level runs.
- **0x006A8BA0** per level: its two light solvers (vfunc +0xC of `this+0x290` / `+0x2E8`) and the dirty rooms
  (0x006A88B0 -> ... -> 0x006A3C90), all with (stopwatch, budget). 0x006A3C90 is a resumable state machine (state at
  room+0xEC, 9 = done): while room+0x164 is set it stops as soon as elapsed >= budget and continues next frame; rooms
  without +0x164 finish in one go. A smaller budget spreads the work; it skips nothing.
- **0x00ADB120** budget, thiscall, ret, result in ST0 (its only caller is 0x00ADB95D):

| Case | Test | Budget |
|---|---|---|
| Tool mode | WorldManager `[0x011ECBC4]` +0x1B4 == 0 | `[0x011833DC]` = 1000 ms |
| Priority lot, loading | 0x006FDC80(SceneObjectManager, lot id) and manager +0x4F | `[0x011833D8]` = 30 ms |
| Priority lot | 0x006FDC80 true | `[0x00F9A62C]` = 15 ms |
| Other lot, loading | manager +0x4F | `[0x01045CCC]` = 10 ms |
| Other lot | | `[0x00FBD498]` = 5 ms |

- Lot id = `[[manager+0x14]+0x48/+0x4C]`. Priority lots = the two lot ids at SceneObjectManager (`[0x011D1CF8]`)
  +0x10D0 / +0x10E0, copied from +0x10D8 / +0x10E8 in 0x00701270 (writers 0x00703510, 0x00703D60, 0x007042F0, all
  renderer / object-hiding code): most likely the active or focused lot (inferred, not proven). The measured 15 to 17 ms
  "Lot room solve" hitches are this 15 ms budget.
- The budget function's `+0x4D` flag is the lot thumbnail's forced quality (0x00ADC180 from UI/ThumbnailManager), not a
  Build mode flag.

### The patch

- The 5 bytes of the CALL at 0x00ADB95D become `call Hook_LotLightBudget` (`MemPatch::WriteCodeSuspended`; restored the
  same way, only if it still points to Apex's hook). Start checks that the CALL reaches 0x00ADB120 and that the next
  instruction reads ST0 (`D9` / `DD`).
- `float __fastcall Hook_LotLightBudget(mgr, edx)` calls 0x00ADB120 (a float return is ST0 in every x86 convention; the
  caller's x87 stack is empty at the call), samples the camera, and applies the rules above. The minimum is 0.25 ms
  (`kMinMs`); the lamp edit and after-load budget is `kLampEditMs` (25 ms), the refinement cap `kRefineMs` (6 ms), the edit room's budget while moving `kEditMovingMs` (6 ms; a room counts as
  urgent when `LevelLightShare::LampUrgency` of the queue's current room is above 1). The drag spread cap is checked
  first, before the switch's own rules, and is `DragLite::kSpreadCapMs` (3.5 ms) for at most `kSettleMs` (15 s). The
  steady pacing applies to budgets equal to `kPriorityMs` (15 ms) or `kOtherLotMs` (5 ms); `SteadyScale()` (1 with the
  Tester switch off) and `SteadyDrainMs(wanted)` = `1 + (wanted - 1) x SteadyScale()` give it to the room queue.
- **Lot build slice** (optional part; skipped with a warning when the code differs): 0x00AEA680 stores its slice budget
  at `[esp+14h]` (20 ms, 35 for a priority lot), then tests `[[0x011ECBC4]+0x1B4]` for the loading screen's 2000 ms. The
  5-byte `mov eax,[0x011ECBC4]` at 0x00AEA6D8, right after the budget is chosen, becomes a CALL of `LotBuildStub`: it
  lowers the budget to 6 ms (`kBuildMovingMs`) while the camera moves (only budgets above 6 and under 1000 ms) and loads
  eax as the original did, so the 2000 ms test still runs after it. Stop writes the original bytes back.

### Camera motion

The camera eye is `[[root]+0x24]+0x60`, the read WorldManager::Update does at 0x00C6D5BD..0x00C6D5C9
(`call 0x006E8330` = `mov eax,[0x011D1860]; ret`, `call 0x006E8400` = `mov eax,[ecx+24h]; ret`, `movaps xmm0,[eax+60h]`;
its y is compared with the terrain height threshold WorldManager+0xE8). The root global, the camera offset and the eye
offset are parsed from those bytes at Start on every build (`EnsureCamera`, once, any thread: Start, StartWallAo or the
first `SampleCameraMoving`); the getters must have the shapes `A1 imm32 C3` and `8B 41 disp8 C3`, and the eye read
`0F 28 40 disp8`.

- Moving = any eye component changed by more than 5 mm since the previous sample, or by more than 5 mm within 100 ms
  (slow pans and zooms). It lasts 300 ms after the last change (`kHoldMs`). Orbit, zoom and pan all move the eye;
  following a walking Sim does too.
- The read is SEH-guarded; not readable (no world) = not moving.
- Sampled inside the hook and once per frame from a Present callback (`D3D9Hooks::RegisterPresent("LotLightingMotion")`,
  registered while this feature or Wall Shading While Moving is on), so a stale eye after a quiet spell does not read as
  a false "moving".

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| LotLightBudgetCall / LotLightBudget | 0x00ADB95D / 0x00ADB120 | Sig / Target (fallback Sig) |
| CameraRootCall / CameraGetterCall | 0x00C6D5BD / 0x00C6D5C4 | Sig (+0 / +7) |
| CameraRootGetter / CameraGetter | 0x006E8330 / 0x006E8400 | Target |
| LotBuildBudgetLoad | 0x00AEA6D8 | Sig (+0x2C) |

Group `LotLightingMotion`.

### Source

`features/lot_lighting_motion.{h,cpp}`: `Start`, `Stop`, `Hook_LotLightBudget`, `SampleCamera`, `ReadEye`,
`ParseCamera` (`ParseRootGetter` / `ParseCameraGetter` / `ParseEyeRead`), `UpdatePresentSampler` / `OnPresentSample`,
`SetBudgetMs`, `CameraMoving`, `SampleCameraMoving`, `EnsureCamera`, `PaceSettle`, `SettleScale`, `SteadyScale`,
`SteadyDrainMs`, `DrainBudget`, `StatusText`, `RenderDeveloperUI`.
`Performance::LotLightingBudgetMs` / `SetLotLightingBudgetMs` in [`patches/performance.h`](../../../patches/performance.h).

Developer card *Lighting while the camera moves*: the call and camera addresses, "Camera moving" / "Camera still", the
last camera move, frames sampled, lot build slices (shortened while moving, on / off), budget calls, calls scaled while
moving (a lamp edit's room kept up to 6 ms; settle or lamp switch paced), camera reads failed and the last budget ("game 15.00 ms -> 3.00 ms"),
and "Steady pacing (camera still): N% of the game's budgets (frame baseline X ms), lowered in N budget calls".

## Rejected approaches

- A fixed smaller lot lighting budget with the camera still too: lamps moved in Build mode updated visibly slower for a
  small gain. Do not bring it back without a real Build-mode or lamp-moved signal. The steady pacing above is not that
  approach: it keeps the game's budget while frames hold and leaves a lamp edit's rooms their own budgets. Details in
  [history](../../history/performance-lot-lighting-motion.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-lot-lighting-motion.md)
- [History](../../history/performance-lot-lighting-motion.md)
- [Lot loading and streaming](../../engine/lot-loading-and-streaming.md), [room light maps](../../engine/room-light-maps.md)

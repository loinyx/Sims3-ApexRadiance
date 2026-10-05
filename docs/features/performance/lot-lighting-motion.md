# Lot Lighting While Moving

While the camera moves, lots relight in small steps each frame instead of taking up to 15 ms at once, so panning over
busy neighbourhoods stutters less. Nothing is skipped: the room solves resume on the next frame, and lights finish at
the game's own pace as soon as the camera stops.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (on by default since its introduction) |
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
  caller's x87 stack is empty at the call), samples the camera, and applies the rule above. The minimum is 0.25 ms
  (`kMinMs`).

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

Group `LotLightingMotion`.

### Source

`features/lot_lighting_motion.{h,cpp}`: `Start`, `Stop`, `Hook_LotLightBudget`, `SampleCamera`, `ReadEye`,
`ParseCamera` (`ParseRootGetter` / `ParseCameraGetter` / `ParseEyeRead`), `UpdatePresentSampler` / `OnPresentSample`,
`SetBudgetMs`, `CameraMoving`, `SampleCameraMoving`, `EnsureCamera`, `StatusText`, `RenderDeveloperUI`.
`Performance::LotLightingBudgetMs` / `SetLotLightingBudgetMs` in [`patches/performance.h`](../../../patches/performance.h).

Developer card *Lighting while the camera moves*: the call and camera addresses, "Camera moving" / "Camera still", the
last camera move, frames sampled, budget calls, calls scaled while moving, camera reads failed and the last budget
("game 15.00 ms -> 3.00 ms").

## Rejected approaches

- A smaller lot lighting budget with the camera still too: lamps moved in Build mode updated visibly slower for a small
  gain. Do not bring it back without a real Build-mode or lamp-moved signal. Details in
  [history](../../history/performance-lot-lighting-motion.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-lot-lighting-motion.md)
- [History](../../history/performance-lot-lighting-motion.md)
- [Lot loading and streaming](../../engine/lot-loading-and-streaming.md), [room light maps](../../engine/room-light-maps.md)

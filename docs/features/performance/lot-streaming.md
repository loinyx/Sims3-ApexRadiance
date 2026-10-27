# Lot Streaming

Lot Streaming lets nearby lots keep their full detail farther from the camera and lets more of them be detailed at once,
and makes them stream in smoothly while the camera moves: fewer lots switch detail back and forth, lots at the edge of
the detail distance keep their detail instead of flickering between detailed and simple, and lot streaming pauses while
the map view is open. Lots that load together share one build time per frame, so a burst of lots no longer stalls a
frame. A separate switch builds a lot's objects a few at a time. Research, measurements and code by **idavidveiga**;
detailed-lot hysteresis, throttle threshold follow and shared build time by @loinyx.

## Status

| | |
|---|---|
| Availability | Experimental: Released in 2.7.0. Detailed-lot hysteresis, throttle threshold follow and shared lot build time: In development (branch `perf/lot-streaming`) |
| Default | Off (every switch); the shared lot build time is part of Lot Lighting While Moving, on by default |
| Menu | System > Lot Streaming (cards *Lot detail streaming* and *Object streaming*) |
| Configuration | `[patches.LotDetailRange]`, `[patches.LotLodStreaming]`, `[patches.LotVisibilityOverride]`, `[patches.MapViewStreamingBlocker]`, `[patches.LotObjectThrottle]` in `ApexRadiance.toml` |
| Source | [`features/lot_detail_range.{h,cpp}`](../../../features/lot_detail_range.cpp), [`features/lot_lod_streaming.{h,cpp}`](../../../features/lot_lod_streaming.cpp), [`features/lot_visibility_override.{h,cpp}`](../../../features/lot_visibility_override.cpp), [`features/lot_object_throttle.{h,cpp}`](../../../features/lot_object_throttle.cpp), [`features/lot_active_threshold.{h,cpp}`](../../../features/lot_active_threshold.cpp), [`features/lot_build_share.{h,cpp}`](../../../features/lot_build_share.cpp) with [`lot_build_share_policy.h`](../../../features/lot_build_share_policy.h), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game decides lot detail in several separate steps, each with its own native value: whether a lot is close enough
for Detailed View (Lot LOD distance, 70 by default), how many lots may stay detailed at once (Max Active Lots, 8), how
many detail transitions may start together (a native throttle, off, with a camera-speed threshold of 32), how much
longer a detailed lot keeps its detail (Active Lot Bias), and how a promoted lot's objects are built (all in one burst).
At the game's values few lots are detailed, and while the camera moves lots switch detail back and forth. See
[engine/lot-loading-and-streaming.md](../../engine/lot-loading-and-streaming.md).

Three native details make it worse than it looks:

- The lot metric is a squared distance from the camera's target point (no view angle), and Active Lot Bias (8.0) is
  subtracted from it: for a detailed lot that is under 3 m of distance, so the native hysteresis does almost nothing.
- The transition throttle applies only while Max Active Lots is at most "Max Active Lot Threshold" (12): with more
  detailed lots it silently turns off.
- Each loading lot runs its load stages with its own 20 ms slice (35 for the lot being played), so lots that load
  together add up in one frame. Recorded loads show up to 7 lots building in one frame and 96 hitches dominated by the
  load stages (frames up to 134 ms).

## How Apex Radiance solves it

Each step is a separate switch that sets the game's own value or flag; no lot loader is replaced.

1. **Extended lot detail** sets the Lot LOD distance (`WorldManager+0xDC`) and Max Active Lots (`+0xE4`) of each live
   WorldManager to the chosen values (300 and 16 by default; the game's metric is a squared distance).
2. **Smooth lot streaming** turns on the game's native "Throttle Lot LoD Transitions" flag and sets its camera-speed
   threshold (`+0xEC`) to 5.0.
   While it runs, the "Max Active Lot Threshold" follows Max Active Lots (never below its own value), so the throttle
   still applies with Extended lot detail's 16 lots.
3. **Keep lot visibility stable** gives detailed lots a real hysteresis: the metric's subtraction of Active Lot Bias is
   replaced by a multiplication by 0.81 for lots already detailed. A detailed lot keeps its detail until it is about
   11% farther than where a new lot is promoted (333 m at a distance of 300) and wins capacity ties against a lot only
   slightly nearer. Lots not detailed are measured as before.
4. **Pause lot streaming in map view** holds the live "skip lot streaming" gate (`+0x258`) while the map view is open
   and for 1 s after it closes, without detouring WorldManager::Update.
5. **Spread lot objects while loading** replaces `Lot::AddLotObjectsToScene` so regular objects are built a few per
   window, the next window posted through the game's own remote-method call. Building and apartment shells, large
   exterior geometry and flora are built in the first window, because the lot's activation fix-ups need them at once.

Every write is guarded: Apex reads the expected native value first, keeps a value only while it still owns it, yields to
a value another mod set, and restores only what it changed.

### Shared lot build time

Started with Lot Lighting While Moving (on by default; development switch `lot_build_share`). The load stages of every
streaming lot share one build budget per frame: 6 ms with the camera still, 4 ms while it moves. Each lot's call gets
what is left of the budget as its slice. The lot being played always runs, with at least half the budget. Another lot
whose turn comes with less than 1 ms left waits for the next frame, the same answer the stages give when the lot's lock
is busy; after 6 frames of waiting it runs one stage, one such lot per frame. Stages 17 and later only wait for a job or
for the lot's lighting and always run, so no lot finishes later because of a wait. The loading screen and the impostor
builder's synchronous updates are left alone. A lot's build ends a few frames later; the replay of recorded loads is in
the [validation page](../../validation/performance-lot-streaming.md#latest-results).

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Extended lot detail (Experimental) | `[patches.LotDetailRange] enabled` | bool | off | | More lots in full detail, farther away, than the game's 70 / 8 |
| Lot detail distance (shown while on) | `[patches.LotDetailRange] distance` | int | 300 | 70 to 300, steps of 10 | How far lots stay in full detail; 70 is the game's. Applied live |
| Maximum detailed lots (shown while on) | `[patches.LotDetailRange] maxActiveLots` | int | 16 | 8 to 16 | How many lots can be detailed at once; 8 is the game's. More lots use more memory. Applied live |
| Smooth lot streaming (Experimental) | `[patches.LotLodStreaming] enabled` | bool | off | | The native transition throttle and a camera threshold of 5.0 |
| Keep lot visibility stable (Experimental) | `[patches.LotVisibilityOverride] enabled` | bool | off | | Lots in full detail keep it about 11% farther, so lots at the edge stop switching back and forth |
| Pause lot streaming in map view (Experimental) | `[patches.MapViewStreamingBlocker] enabled` | bool | off | | No lot detail streaming while the map view is open |
| Spread lot objects while loading (Experimental) | `[patches.LotObjectThrottle] enabled` | bool | off | | Builds a lot's objects a few at a time |
| Objects per lot window (shown while on) | `[patches.LotObjectThrottle] objectsPerLot` | int | 2 | 1 to 64 | Regular objects built per window; 2 matches Sims3SettingsSetter |
| Delay between lot windows (shown while on) | `[patches.LotObjectThrottle] delayMs` | int | 16 ms | 0 to 500 ms | Minimum wait between two windows of the same lot |
| (no menu row) | `[patches.LotActiveThreshold] enabled` | bool | off | | EA app build only: sets the internal transition threshold "Throttle Lot LoD Transitions Max Active Lot Threshold" to 12, as Sims3SettingsSetter does. Not Max Active Lots |

A missing key reads as off. None of these switches is part of a profile part, and the built-in profiles leave them as
the player set them.

## Compatibility and interactions

- **Official Sims3SettingsSetter:** when its LotStreamingOptimizations has the matching setting on
  (`streamingSettings`, `mapViewBlocker`, `visibilityOverride`, `objectThrottle`), that part makes no write and its row
  shows "Handled by Sims3SettingsSetter". A visibility branch already patched by another mod is left alone ("Already
  applied by another patch").
- **Spread new objects over frames** ([scene-node-budget.md](scene-node-budget.md)) works later in the pipeline (the
  scene's pending-node drain); the two are independent and can be on together.
- **Faster room lighting and lamp edits:** lamps are lot objects. With *Spread lot objects while loading* on they arrive
  one by one and every arrival relights the rooms, so the lighting of a lot settles later.
- **Memory:** more detailed lots use more of the 32-bit game's address space; see *Room to save*
  ([room-to-save.md](room-to-save.md)).

## Limitations

- Distance 300 and 16 lots are the tested maximums; the menu does not go higher.
- The Steam addresses of Extended lot detail, the visibility branch and the object throttle were found offline with the
  fork's EA signatures; the research and the controlled tests were made on the EA app build 1.69.
- *Spread lot objects while loading* makes objects, lamps included, appear over a few frames when a lot is promoted.
- A single load stage can take longer than the shared budget (stage 0 of a large lot took up to 68 ms in recordings);
  the shared build time cannot split a stage.
- Most of a lot's time to full detail is the lighting wait of stage 20 (median 1.9 s of 2.3 s in recordings), which the
  build share does not change ([engine page](../../engine/lot-loading-and-streaming.md#stage-20-the-lighting-wait)).

## Technical reference

| Part | Game side (Steam 1.67.2) | What Apex writes |
|---|---|---|
| Extended lot detail | WorldManager `[0x011ECBC4]`: `+0xDC` Lot LOD distance (70), `+0xE4` Max Active Lots (8); `+0xE0` Active Lot Bias (8.0) and `+0xE8` terrain-height threshold (600) are left alone | The two values per live WorldManager, captured first and maintained by `Tick` while still owned |
| Smooth lot streaming | Throttle test 0x00C6C695 (inside LotLodScoring 0x00C6C290) on the flag byte 0x011ECBC0; `+0xEC` camera-speed threshold (32); `cmp edx,[0x01184328]` at 0x00C6C6B7 (Max Active Lots against the threshold, 12) | Flag on; `+0xEC` = 5.0 after checking a finite value in 0 to 100; the threshold raised to Max Active Lots while it is lower, restored only while it holds Apex's value |
| Keep lot visibility stable | `JZ` (0x74) at 0x00C63015 in the lot metric 0x00C62D80, then `subss xmm0,[edi+0E0h]` (8 bytes at 0x00C6301D) | That instruction becomes `mulss xmm0,[factor]` (0.81), only when the JZ reads 0x74 and the 8 bytes match |
| Shared lot build time | CALL 0x00AEB306 (renderer update 0x00AEB2E0 -> load stages 0x00AEA680, result unused); lot pass CALL 0x00C7CEDF; slice load 0x00AEA6D8 | `CallChain` layer `LotBuildGate` (outermost) on 0x00AEB306; the slice cap through Lot Lighting While Moving's stub at 0x00AEA6D8 |
| Pause in map view | `+0x258` "skip lot streaming"; map view from `Camera_IsMapViewModeEnabled` | The gate while the map is open and 1000 ms after |
| Spread lot objects | LotAddObjectsToScene 0x00AC1130, LotUpdateObjectSceneNode 0x00ABFAC0, ScriptMessageScope 0x007D2DB0 / 0x007D2DF0, PostRemoteMethodCall 0x00ABE9C0, IsObjectLargeOrFlora 0x00B088C0 | `EntryChain` layer `LotObjectThrottle` on AddLotObjectsToScene |

Address groups `LotLodStreaming` (scoring, throttle test and flag, WorldManager), `LotLodActiveFollow` (the threshold
compare and its global, optional), `LotVisibilityOverride`, `LotBuildShare` (the two CALLs, the load stages, the
WorldManager and the priority-lot test) and `LotObjectThrottle`; on other builds the same parts resolve by signature and stay off when a signature is missing. The
EA app build 1.69.47 addresses of the throttle test and flag (0x00C6BA15 / 0x01246C50) were verified in game by the
fork. The fork's diagnostic metric probe is not part of Apex Radiance.

## Rejected approaches

- Distances above 300 or more than 16 detailed lots: not exposed until tested for stability and memory.
- The threshold-12 control in the main menu: it is not Max Active Lots and misled players; kept registered for reference
  only.
- Spread lot objects while loading on by default: lamps arriving one by one relit the rooms (5.8 to 7.2 s to settle).
- The visibility override as a `JZ` -> `JMP` (Sims3SettingsSetter's): the metric has no view angle, so it only removed
  the small native hysteresis; replaced by the 0.81 factor.
- A smaller per-lot slice alone: lots that load together still add up in one frame; the budget is shared instead.

Details in [history](../../history/performance-lot-streaming.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-lot-streaming.md)
- [History](../../history/performance-lot-streaming.md) (with the fork's research log)
- [Lot loading and streaming](../../engine/lot-loading-and-streaming.md)

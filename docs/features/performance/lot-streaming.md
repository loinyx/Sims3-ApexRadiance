# Lot Streaming

Lot Streaming lets nearby lots keep their full detail farther from the camera, makes lots switch between full detail
and impostor less while the camera moves, and pauses lot streaming while the map view is open. Lots that load together
share one build time per frame, so a burst of lots no longer stalls a frame. How many lots are detailed at once and how
fast the camera may move before lots stop streaming stay the game's own graphics option. Research, measurements and code
by **idavidveiga**; detailed-lot hysteresis and shared build time by @loinyx.

## Status

| | |
|---|---|
| Availability | Experimental: Released in 2.7.0. Detailed-lot hysteresis and shared lot build time: Released in 2.16.2; hysteresis factor 0.5 instead of 0.81 in 3.1.2. Smooth lot streaming, Spread lot objects while loading, the threshold-12 switch and Maximum detailed lots: Removed in 3.1.2 ([removed features](../../removed-features.md#lot-streaming-parts)) |
| Default | Off (every switch); the shared lot build time is part of Lot Lighting While Moving, on by default |
| Menu | System > Lot Streaming (card *Lot streaming*, group *Lot detail*) |
| Configuration | `[patches.LotDetailRange]`, `[patches.LotVisibilityOverride]`, `[patches.MapViewStreamingBlocker]` in `ApexRadiance.toml` |
| Source | [`features/lot_detail_range.{h,cpp}`](../../../features/lot_detail_range.cpp), [`features/lot_lod_streaming.{h,cpp}`](../../../features/lot_lod_streaming.cpp) (the map-view pause), [`features/lot_visibility_override.{h,cpp}`](../../../features/lot_visibility_override.cpp), [`features/lot_build_share.{h,cpp}`](../../../features/lot_build_share.cpp) with [`lot_build_share_policy.h`](../../../features/lot_build_share_policy.h), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game decides lot detail in several separate steps, each with its own native value: whether a lot is close enough
for Detailed View (Lot LOD distance, 70 by default), how many lots may stay detailed at once (Max Active Lots, the
player's graphics option: 8 at most, 4 when the game picks it for the best hardware), how much longer a detailed lot
keeps its detail (Active Lot Bias), and how much build work a loading lot does per frame. At the game's distance few
lots are detailed, and while the camera moves lots switch detail back and forth. See
[engine/lot-loading-and-streaming.md](../../engine/lot-loading-and-streaming.md).

Two native details make it worse than it looks:

- The lot metric is the squared distance from the camera's target point to the lot's rectangle (the largest axis
  distance; no view angle). The lots are ranked by it, and for a detailed lot Active Lot Bias (8.0) is subtracted
  first: under 3 m of distance, so lots competing for the Max Active Lots slots swap back and forth as the camera moves.
  The 0.81 factor Keep lot visibility stable used until 3.1.1 did not stop it: a recorded session at distance 300 with
  it on asked for the same lot 12 times in 96 s, and 13 of 31 promoted lots were demoted before their build finished.
- Each loading lot runs its load stages with its own 20 ms slice (35 for the lot being played), so lots that load
  together add up in one frame. Recorded loads show up to 7 lots building in one frame and 96 hitches dominated by the
  load stages (frames up to 134 ms).

## How Apex Radiance solves it

Each step is a separate switch that sets the game's own value or code; no lot loader is replaced.

1. **Extended lot detail** sets the Lot LOD distance (`WorldManager+0xDC`) of each live WorldManager to the chosen value
   (300 by default; the game's metric is a squared distance). Max Active Lots (`+0xE4`) and the camera-speed threshold
   (`+0xEC`) stay the values the game applies from its own option at the end of each world load.
2. **Keep lot visibility stable** gives detailed lots a real hysteresis: the metric's subtraction of Active Lot Bias is
   replaced by a multiplication by 0.5 for lots already detailed. Only the ranking reads that value: when more lots are
   in range than the game keeps detailed, a detailed lot keeps its slot until a waiting lot is about 1.41 times nearer
   (the square root of 2). The game's distance test still reads the raw distance, so no lot without a game priority is
   detailed beyond the detail distance and the number of detailed lots stays the game's.
3. **Pause lot streaming in map view** holds the live "skip lot streaming" gate (`+0x258`) while the map view is open
   and for 1 s after it closes, without detouring WorldManager::Update.

Every write is guarded: Apex reads the expected native value first, keeps a value only while it still owns it, yields to
a value another mod set, and restores only what it changed.

### Shared lot build time

Started with Lot Lighting While Moving (on by default; development switch `lot_build_share`). The load stages of every
streaming lot share one build budget per frame: 6 ms with the camera still, 4 ms while it moves. Each lot's call gets
what is left of the budget as its slice. The lot being played always runs with the game's own slice; its time counts
against the budget. Another lot whose turn comes with less than 1 ms left waits for the next frame, the same answer the
stages give when the lot's lock is busy; once it has waited 6 frames and longest of the waiting lots it runs one stage,
one such lot per frame, so with N lots waiting none waits more than 5 + N frames. Stages 17 and later only wait for a job
or for the lot's lighting and always run. Only the lot pass's own updates are gated: a second gate layer on its CALL
(0x00C7CEDF) marks them, so the impostor builder's synchronous updates, the loading screen and a detour of the renderer
update's entry (the Frame Profiler's) leave it working. Without Lot Lighting While Moving's slice stub the share stays
off. A lot's build ends a few frames later; the replay of recorded loads is in
the [validation page](../../validation/performance-lot-streaming.md#latest-results).

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Extended lot detail (Experimental) | `[patches.LotDetailRange] enabled` | bool | off | | Lots stay in full detail farther away than the game's 70 |
| Lot detail distance (shown while on) | `[patches.LotDetailRange] distance` | int | 300 | 70 to 300, steps of 10 | How far lots stay in full detail; 70 is the game's. Applied live |
| Keep lot visibility stable (Experimental) | `[patches.LotVisibilityOverride] enabled` | bool | off | | When more lots are in range than the game keeps in full detail, a lot in full detail keeps it until a lot about 1.4 times nearer needs it, so lots swap less while the camera moves |
| Pause lot streaming in map view (Experimental) | `[patches.MapViewStreamingBlocker] enabled` | bool | off | | No lot detail streaming while the map view is open |

A missing key reads as off. None of these switches is part of a profile part, and the built-in profiles leave them as
the player set them. The keys of the removed parts (`[patches.LotLodStreaming]`, `[patches.LotObjectThrottle]`,
`[patches.LotActiveThreshold]` and `[patches.LotDetailRange] maxActiveLots`) are ignored when a configuration loads.

## Compatibility and interactions

- **Official Sims3SettingsSetter:** when its LotStreamingOptimizations has the matching setting on (`mapViewBlocker`,
  `visibilityOverride`), that part makes no write and its row shows "Handled by Sims3SettingsSetter". A visibility
  branch already patched by another mod is left alone ("Already applied by another patch").
- **The game's graphics option for detailed lots:** it sets Max Active Lots and the camera-speed threshold together, and
  the game applies it at the end of each world load. Apex never writes either value.
- **Memory:** a longer distance keeps more lots detailed, up to the game's Max Active Lots, and uses more of the 32-bit
  game's address space; see *Room to save* ([room-to-save.md](room-to-save.md)). Keep lot visibility stable changes
  only which lots are detailed, never how many or how far.

## Limitations

- Distance 300 is the tested maximum; the menu does not go higher.
- How many lots are detailed at once is the game's option (8 at most); Apex does not raise it.
- The Steam addresses of Extended lot detail and the visibility branch were found offline with the fork's EA
  signatures; the research and the controlled tests were made on the EA app build 1.69.
- A single load stage can take longer than the shared budget (stage 0 of a large lot took up to 68 ms in recordings);
  the shared build time cannot split a stage.
- Lots at the edge of the detail distance still switch as natively: the hysteresis reaches only the ranking (see the
  rejected distance-test change below).
- With the 0.5 factor a detailed lot may stay ahead of a waiting lot up to 1.41 times nearer, and it keeps that place
  while the camera stays still (a nearer lot waited in 61% of the town model's frames, 14% with 0.81). The lot under the
  camera target is detailed whenever the game gives it priority (inferred).
- Most of a lot's time to full detail is the lighting wait of stage 20 (median 1.9 s of 2.3 s in recordings), which the
  build share does not change ([engine page](../../engine/lot-loading-and-streaming.md#stage-20-the-lighting-wait)).

## Technical reference

| Part | Game side (Steam 1.67.2) | What Apex writes |
|---|---|---|
| Extended lot detail | WorldManager `[0x011ECBC4]`: `+0xDC` Lot LOD distance (70.0 from the constructor). `+0xE4` Max Active Lots, `+0xEC` camera-speed threshold (both from the game's option at load end, 0x00C60C00), `+0xE0` Active Lot Bias (8.0) and `+0xE8` terrain-height threshold (600) are left alone | `+0xDC` per live WorldManager, captured first and maintained by `Tick` while still owned |
| Keep lot visibility stable | `JZ` (0x74) at 0x00C63015 in the lot metric 0x00C62D80, then `subss xmm0,[edi+0E0h]` (8 bytes at 0x00C6301D) on the value it returns (entry `+0xC`, ranked by 0x00C64050); the raw value goes to its out pointer (entry `+4`, read by the distance test `comiss xmm0,[edi+4]` at 0x00C6C706) | That instruction becomes `mulss xmm0,[factor]` (0.5), only when the JZ reads 0x74 and the 8 bytes match; the distance test stays native |
| Shared lot build time | CALL 0x00AEB306 (renderer update 0x00AEB2E0 -> load stages 0x00AEA680, result unused); lot pass CALL 0x00C7CEDF; slice load 0x00AEA6D8 | `CallChain` layer `LotBuildGate` on 0x00AEB306 (the gate) and on 0x00C7CEDF (the lot pass marker); the slice cap through Lot Lighting While Moving's stub at 0x00AEA6D8 |
| Pause in map view | `+0x258` "skip lot streaming"; map view from `Camera_IsMapViewModeEnabled` | The gate while the map is open and 1000 ms after |

Address groups `LotLodStreaming` (the scoring and the WorldManager), `LotVisibilityOverride` and `LotBuildShare` (the
two CALLs, the load stages, the WorldManager and the priority-lot test); on other builds the same parts resolve by
signature and stay off when a signature is missing. The fork's diagnostic metric probe is not part of Apex Radiance.

## Rejected approaches

- Distances above 300: not exposed until tested for stability and memory.
- Smooth lot streaming (the native transition throttle, camera threshold 5.0): it promoted one lot at a time, and turned
  off in a loaded world it wrote back the constructor's threshold 0.0, which stopped lot streaming while the camera
  moved. Removed in 3.1.2.
- Maximum detailed lots (Max Active Lots up to 16): turned off in a loaded world it restored the constructor's 4 instead
  of the player's option. Removed in 3.1.2; the game's option always applies.
- Spread lot objects while loading: lamps arriving one by one relit the rooms (5.8 to 7.2 s to settle), and the
  per-object work it spread cost 3.3 ms over 6075 calls. Removed in 3.1.2.
- The threshold-12 switch (`[patches.LotActiveThreshold]`, EA app only, no menu row): it set "Max Active Lot Threshold"
  to 12, not Max Active Lots, and did nothing on Steam. Removed in 3.1.2.
- The visibility override as a `JZ` -> `JMP` (Sims3SettingsSetter's): the metric has no view angle, so it only removed
  the small native hysteresis; replaced by the factor.
- The 0.81 factor (2.16.2 to 3.1.1): it changed only the ranking, not the distance test, so it never kept a lot detailed
  farther; a detailed lot held its slot only against lots up to 1.11 times nearer. Replaced by 0.5 (11% to 49% fewer
  switches in the model where the 8 slots are the limit). 0.25 cuts more but lets a lot twice as far keep its slot.
- The distance test on the ranked value (1 byte at 0x00C6C709, `04` -> `0C`), for a hysteresis at the edge of the
  distance: a detailed lot up to 1.41 times past the distance kept a slot that a waiting lot inside it needed, so that
  nearer lot stayed an impostor without its lighting. In the scoring model this happened in 18% of the frames of a
  sparse grid (lots 100 m apart, at 120 or 150 m) and 80% with lots 200 m apart at 300 m; the raw test never allows it.
  Rejected in 3.1.2 before release.
- A smaller per-lot slice alone: lots that load together still add up in one frame; the budget is shared instead.

Details in [history](../../history/performance-lot-streaming.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-lot-streaming.md)
- [History](../../history/performance-lot-streaming.md) (with the fork's research log)
- [Lot loading and streaming](../../engine/lot-loading-and-streaming.md)
- [Removed features: Lot Streaming parts](../../removed-features.md#lot-streaming-parts)

# GPU room lamps: history

Dated investigations behind [GPU room lamps](../features/night-lighting/gpu-room-lamps.md). Test results are on the
[validation page](../validation/night-lighting-gpu-room-lamps.md).

### 2026-11-03: game solves during a GPU drag

*Context:* in the physical model the game kept solving a dragged lamp's rooms: 200 kept solves in 40 s and 2,695 game writes over
the GPU's maps in one recording. *Finding:* the leaks were the lamp's own occluder marks, the walk's tail rooms, `SolveUnkept` and
stranded rooms raised by `Prioritise`. *Outcome:* `DragLite::HoldGpuDrawn` runs before `Prioritise`, about 3,900 picks held per
10 s. A version that also held rooms drawn in the last 1.5 s was rejected: a room with no kept copy makes `EditRefusal` refuse every
step. Bypassing `AtriumHold` for Apex's own writes was rejected: it would drop them from `m.latest` at release.

### 2026-11-04: tick stamps in the future

*Context:* 6 to 14 solves per 10 s still started in held rooms. *Finding:* stamps written `GetTickCount() | 1` sat 1 ms in the future
for the rest of an even tick, so unsigned "within ms" reads wrapped to false. *Outcome:* `std::max<DWORD>(tick, 1)` at 35 sites of
`features/`; `(now + X) | 1` deadlines with signed readers stay. 0 solves escaped the hold in the next recording.

### 2026-11-04: furniture tone at a keep

*Context:* furniture changed tone after the release. *Finding:* the kept base, normalisation, ambient, objects map and rig were flat
at a keep; the step was in the directional maps, whose basis has no furniture test (game 1.00x vis, 1.4 to 1.86x vis x t3d). Removing
t3d left steps of -23% and +27% in story 1 room 2. *Outcome:* a moved lamp's part comes from the solve's own code
(`LevelLightShare::BasisPartNow`). The directional t3d was removed later. `g_basisCarried` stays: `r.tests` feeds `CarryStale`.

### 2026-11-05: metal and rig spikes

*Context:* metal followed the lamp 4.5 s after the drop, and rigs spiked x1.2 to 1.6 at regather ticks. *Finding:* the spikes were parts
of the dragged lamp, lit by its own bulb: the rig test lacked the game's own-light exclusion. The cube of metal is a room LightProbe
taken only 2 s after a solve starts, and a GPU drag runs none (cause plausible, not proven). *Outcome:* `RigLightTestAsRoom` honours
`light+0xB4 == rig+0x1E4`; `ReflectionDue` re-arms the probe, about 7 frames a room. "Metal reads the old map" was refuted: the
SetTexture swap covers all draws.

### 2026-11-05: lighter drag rounds

*Context:* a recording had 29 ms of GPU a round; `AccPhys` took 73%, `T3dPS` 21%, the compose 5%. *Outcome:* dense quads and a 3D test
only where `AccPhys` reads it, 10.9 to 7.5 ms on the benchmark, well short of the forecast 5 to 8 times: a small map's draw is bound by
its slowest texel's walk. Going further needs batching across maps, with merged blocker tables and lamp lists.

| Rejected | Evidence |
|---|---|
| Ring of room tables | 7.43 against 7.47 ms |
| Rotating sum and 3D test targets | 8.43 against 8.54 ms |
| All rotated together | 6.99 ms in the best of 3 runs (others 14.8 and 18.3), about +30 MB |
| Blocker texture kept per room | 8.27 against 8.54 ms |
| Still lamps' cached sum | Upper bound 7.5 to 5.4 ms, with a stale light risk |

### 2026-11-05: on by default

*Context:* the capture was behind `kDevToolsBuild && !kPublicBuild`, table legs behind `!kPublicBuild`. *Outcome:* both gates removed;
capture on, physical model and sharper floors by default; true-size shadows stay off for the soft ceiling lamp shadows. The review
found: 3D blocker vtables and test constants are fixed Steam addresses, so non-Steam builds would lose furniture shadows
(the capture now needs `GameAddr::IsFixed()`); Instant to Classic kept up to 192 MB (`KeptClearDue`); Night Lights off and on drew stale
rooms (`Uninstall` turns the capture off); a world change kept the old rooms (`OnWorldChanged` clears them).

### 2026-11-05: light gone from the windows after a while

*Context:* in a long session the indoor light stopped leaving through the windows, and Build mode showed nothing live. *Finding:* no map,
counter or limit failed. At 5 a.m. the sky's night level fell from 1 to 0 in under 0.6 s, with the sun still 6.9 degrees under the horizon,
so outdoor lamp terms went to their daytime 3%; Build mode stops the clock, so the light stayed there. A fade on the sun's height alone
would have left lamps at 18% in full sun, and the sun's term alone still dropped from 1 to 0.4 in about 0.4 s. *Outcome:*
`TerrainLightingPolicy::LampLevel` plus `LampLevelStep`, in real time so a clock stopped by Build mode cannot leave the level halfway. The
largest lamp step in a frame goes from 0.040 to 0.003. The readers of the sky's own level moved to `g_skyNight`.

### 2026-11-05: long-session audit

*Context:* the same report asked whether anything else could stop the GPU's light. *Finding:* five stops that lasted until a solve, a lot
change or a device reset: failed maps, shader objects after 5 failures, room 0 after 3 asks, `g_budgetOut`, and `kMaxWatch` 64 against a
round of 87 maps. *Outcome:* the table in the feature page's *Long sessions*. Read with nothing to fix: lot change, world change
(`TrackerLive`), device reset (`ReleaseDefault`, `blkBound_`), the owed sets on the light tree thread, the log caps. `MemoryGuard` was only
searched: it has no calls into the lighting code. No save-specific path was found in the lighting modules, and saving was not exercised.

### 2026-11-05: sofa arms and objects in a drag

*Context:* thin streaks past sofas, and object shadows behind a dragged object while lamp shadows followed at once. *Finding:* the game
holds a box-type seat's arms as zero-thickness sheets at the box's full height, so the streak is in its data, not a table bug. A first arm
rule also cut arms that were right; the strict rule hits 22 of 72 slant records. The game writes a blocker's fields during the drag, and the
table only took them at the solve, 1.1 to 1.4 s late. Real meshes for seats were rejected for cost. *Outcome:* `DrawnBlockers` with
`kCapMark`, and `LiveMoves` with a 100 ms limit on `LiveGen`, since each read relights the room. The plan's log-only step and the carry's
removal both wait for `[LiveTable]` data.

### 2026-11-05: release review

*Finding:* `PaneCount` compiled to `rep i0` with `i0 = 255`, so a lot past 255 glazed spans stopped counting panes in `T3dDensePS`: rooms
behind those windows came out 1/0.765 too bright. Reproduced with 300 spans ahead of the two real ones: 237 texels off the CPU count, none
behind glass. *Outcome:* two nested loops, as `Occlude3D` has; 0 off, 840 texels behind glass, and the check stays in `rt_lamps_test`. The
cache was regenerated. Rejected: struct padding in the signature hashes, as the lamp structs have none.

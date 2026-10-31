# Lot light pass: validation

The feature is described in [features/night-lighting/lot-light-pass.md](../features/night-lighting/lot-light-pass.md).
It must:

- Show no straight cut of street-lamp light at a lot border at night.
- Leave lot edges without a nearby lamp unchanged, and never lower the lamp term below the terrain term.
- Produce, at the lot edge, the same terrain term the world grass shows on the other side.
- Reproduce the previous night output exactly when the daylight term is zero.
- Leave the draw to the game when the c14 mapping, the terrain source or the original c28..c31 read is unavailable.
- Align the lot map only for the exact recognised lot VS.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) (`check.cpp` with `-ReferenceSource`) | Production lot HLSL at night against the previous source; day continuity across forced lot-edge weights with native window/lot scaling at zero; extracted regular lot draw path: atlas/chunk fallback, failed original-constant reads, texture reference balance, restoration of all touched state | `run.ps1 -Captures ... -OutDir ... -ReferenceSource 'previous lot_light_bridge.cpp'` | Native D3D9 GPU |
| `tools/terrain_lighting_test/lot_uv_fixture.cpp` | Lot UV inversion for scales 1/16 to 1/256 over local -1 to 256 m; captured outside point moves from the indoor row to the outside row; shader creation by the device | `run_lot_uv_checks.ps1` | Native D3D9 GPU |
| [`tools/ground_shade_test`](../../tools/ground_shade_test/check.cpp) | `ground_lamp_shadow.h`: the lot build timed on a synthetic 40 m lot (8 lamps, 20 walls, soft and hard edges, turned and not) and, with `-Reference <revision>`, compared bit for bit with that revision's header; `ChunkUpdate` against `ChunkFull` after random lot changes; the byte rounding against `lround` over the range; a lamp in the total only (a moving lamp) never darkens a texel against it tested or left out, and with every lamp moving nothing is shaded | `run.ps1 [-Reference <revision>]` | None (CPU) |
| [`shade_lamps_fixture.cpp`](../../tools/terrain_lighting_test/shade_lamps_fixture.cpp) | The real `ShadeLamps`, `MovedApart` and `DragLite::LampMoving`: a moving lamp first in the total at its place now, a still or let-go one tested at its place now, the list within 5 cm and nothing excluded give the lamps at rest bit for bit, past 32 lamps the tested ones are those at rest less the moving one, the 96-lamp cut keeps the moving ones; the let go ends a move and placement steps do not bring it back | `tools/terrain_lighting_test/run_shade_lamps_checks.ps1` | None |
| `tools/terrain_lighting_test/resource_paths_fixture.cpp` | UV fallback and resource restoration | `run_resource_checks.ps1` | Native D3D9 GPU |
| [`tools/ground_shade_gpu_test`](../../tools/ground_shade_gpu_test/check.cpp) | The production GPU path (`ground_shade_gpu.cpp`, `shaders/ground_shade_hlsl.h`) against `ground_lamp_shadow.h`: per lot the bytes of `ShadeJob::Result`, the stamp weight, the lowest byte and the occlusion query, the lot mask against `MakeMaskTexture`'s texels (rooms, soft room edges, outside); scenes: the 40 m lot turned and not, soft and hard, mesh fences and crowns, the edge rules (a sconce on its own wall, a raised foundation, an upper story, walls over the head and under the ground, a balcony lamp), the *Shadow softness* Tester switch off, an 88 m stress lot, a lot whose walls shade nothing. Chunks: the whole chunk from 15 lots against `ChunkFull` at every level, 39 changed-box updates (a lot moved, new, or shading nothing) against the whole chunk made again and `ChunkFull`, bit for bit at every level, the chunk from the GPU's own lot textures, both chunk formats (L8 and the A8R8G8B8 fallback), and the chunk read as the terrain and road copies read it (red, CLAMP, LINEAR with LINEAR mips at lods 0 to 10) against the CPU's L8 texture | `run.ps1` (DXVK: the game's `d3d9.dll` next to `ground_shade_gpu.exe` in the output folder) | D3D9 GPU |

## Latest results

Release validation for `v2.15.6`: Terrain package layout coverage and exact fence gap reconstruction; unknown layouts retain fallback. The reviewed implementation is `f66e777`;
final release build and gameplay confirmation are tracked with the release tag and PR. Offline results do not establish
an FPS gain, complete climate coverage or resolution of texture allocation stalls.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-31 | `acd352e` (release 2.18.4; code `3171fc1`) | `tools/ground_shade_gpu_test`, `tools/ground_shade_test`, `run_shade_lamps_checks.ps1`, the release build in game | 173, 256 and 26 checks, 0 failures; the 2.18.4 build started and compiled its 10 new shaders in the background (834 ms), then built into the ASI; drags, the let go, a post beside a bush and catalog copies checked in game on developer builds of the same code; published asset SHA-256 `4AB04545759882E5DB8729F5945F717AC8B2EA188F753589824357B16C0EDF9F` | MSVC x86; NVIDIA through DXVK |
| 2026-10-30 | uncommitted on `6db9dfb` (lamp shade on the GPU) | `tools/ground_shade_gpu_test` (`run.ps1`) | 117 checks, 0 failures. Lots: the bytes equal `ShadeJob::Result` but for 3 texels of 1/255 over the 13 scenes (1 on the soft 40 m lot, 2 on the soft stress lot); stamp weight within 4.5e-7 relative on the 40 m lots, 2e-4 on the stress lot (GPU square roots and divisions are not correctly rounded; the bytes are not affected); lowest byte and occlusion query right on every scene, 0 on the lot that shades nothing; masks bit for bit. Chunks: the whole chunk and 39 changed-box updates bit for bit at all 11 levels, against the whole chunk made again and `ChunkFull`, in L8 and in A8R8G8B8; the chunk from the GPU's own lot textures equal to `ChunkFull` of the CPU's shades; the readers' sampling equal to the CPU's L8 texture (0 difference). Timing: GPU 0.05 to 0.07 ms a 40 m lot, 0.24 to 0.28 ms the 88 m stress lot (CPU `ShadeJob` 2 to 5.8 ms and 31 to 44 ms), CPU setup 0.01 to 0.25 ms; a changed box 0.28 to 0.39 ms with the CPU's wait, the whole chunk 0.16 to 0.24 ms (CPU `ChunkFull` 13 ms). Shaders: `ShadePS` 2441 slots (190 ms to compile, without optimisation), `ChunkPS` 528 | Native D3D9, RTX 4070 Ti SUPER |
| 2026-10-30 | uncommitted on `6db9dfb` | `tools/ground_shade_gpu_test` with the game's DXVK `d3d9.dll` | 117 checks, 0 failures; the same differences; GPU 0.05 to 0.07 ms a 40 m lot, 0.23 to 0.27 ms the stress lot; a changed box 0.41 ms (L8) and 0.75 ms (A8R8G8B8) with the wait | DXVK, RTX 4070 Ti SUPER |
| 2026-10-30 | uncommitted on `6db9dfb` | `tools/ground_shade_test` (`run.ps1 -Reference HEAD -Reps 3`) after `LampBox`, `WallBox` and `CrownBox` moved out of `ShadeJob`; `run_shade_lamps_checks.ps1` with the GPU path's `ShadeLamps`; `run_stamped_checks.ps1`, `run_dispatch_checks.ps1`, `run_chunk_map_checks.ps1` | 254 checks, 0 failures (the lot build bit for bit with `HEAD`'s header, 2.0 to 3.1 ms); 20 shade lamp assertions (the GPU path keeps a moving lamp tested at its place now, the total as at rest), 7 stamped, 194 dispatch, 31 chunk map: 0 failures | MSVC x86 |
| 2026-10-30 | uncommitted on `6db9dfb` | Standard and Public Release x86 builds; `tools/i18n_check`; `perl tools/menu_audit.pl .` | Both build with no warnings; i18n OK (2440 keys, 21 languages); menu audit: no new finding | MSVC |
| 2026-10-30 | `38a1c5e` (released as 2.18.3, tag `v2.18.3` on merge `b944d26`, ASI SHA-256 `D2934864D337521EF860617309FA340B12BF37055FC5D38BC3A6E371B2C99D4A`; later commits documentation only) | Standard Release x86 build; Public flavour build; CI build and public text on PR 54; `tools/i18n_check`; `perl tools/menu_audit.pl .`; developer builds of the same code in game | Builds with no errors; CI passed; i18n OK (21 languages, 2436 keys); menu audit: no new findings. In game: sidewalks and lot-edge grass steady with the camera at night in the rain, no cut at a lot edge beside a low wall | In game, backend not recorded |
| 2026-10-30 | `5537a83` | `tools/ground_shade_test` with the chunk shade taken as the most light a lot's walls take over the most light a lot sees (a lot next door that does not know the wall, a wall both lots know, a faint lamp) | 244 passed, 0 failed; a chunk made again over a changed box gives the whole chunk's bytes | MSVC x86 |
| 2026-10-29 | `06a3258` | `tools/lighting_prepare_test` with the overlapped-end rule and `Wall::pass` (built as a DLL) | 36 geometry cases exactly equal to the reference gap search; 16 map slots and 144 wall blur cases pass | CPU, x86 /O2 |
| 2026-10-29 | `59201ad` | One-off timing of `FenceCutout::OpenShare` on random DXT5 levels (built as a DLL) | 0.73 ms a decode at 256 x 256, 2.70 ms at 512 x 512: the share is kept per texture and decoded again only when its size, format or sampled bytes change | CPU, x86 /O2 |
| 2026-10-29 | `d8501f2` | `tools/ground_shade_test` with `-Reference v2.17.2` (built as a DLL: new executables were blocked on the test machine) | 248 checks, 0 failures: with solid walls (`pass` 0) the lot build gives the bytes of `v2.17.2`, soft and hard, turned and not; build time unchanged (3.05 ms against 3.05 ms) | CPU, x86 /O2 |
| 2026-10-29 | `6f12188` (mesh fence open share) | `tools/floor_shade_check` (`run.ps1`) with the fence cutout checks | 10,982 checks, 0 failures: `FindCutout` traces all 59 instanced cutout pixel shaders of the precomp; `OpenShare` gives 0.5 on a half-open texture in DXT1, DXT3, DXT5 and A8R8G8B8; a wall with `pass` 0.8 passes 0.8 in the hard and soft tests | CPU fixtures, D3DCompiler on the precomp |
| 2026-10-29 | `d8501f2` (developer build) | Developer log of the lot shade's time to screen, fence edits and lamp drags beside a brick fence | 388 shade updates: median 233 ms, 90th percentile 266 ms from a change of the inputs to the mask at a draw; 1 to 30 ms on the worker; the fence grid lists only the real gap of the test fence | In game, DXVK |
| 2026-10-27 | `47ec493` (release 2.16.0 candidate; the outdoor scale and its shade) | `fxc /T ps_3_0` on `kReplacementHlsl` (dry and `WET`); `tools/rt_lamps_test` | both compile; the scale gives back the sum's largest channel within 0.41 % on 5,427 texels past 1, colour unchanged on 26,214. In game: the courtyard ground past 1 (F7 "outdoor scale: on"); the strip behind the screen wall after the shade change not yet seen | Native D3D9 (offline device) |
| — | `v2.15.6` (implementation `f66e777`) | `world_terrain_layout_test; lighting_prepare_test` | 405 package terrain pairs checked; exact gap and cell results; unknown or capacity-limited paths retain fallback | CPU fixtures; native D3D9 shader creation |
| 2026-10-23 | `40f351e` | `tools/ground_shade_test` (`run.ps1`) | 240 checks, 0 failures: the whole chunk gives the bytes it gave before; after each of 240 random lot changes (added, moved, reshaded, gone, turned grids, over the chunk's edges) and then every lot gone, `ChunkUpdate` gives `ChunkFull`'s bytes at every mip; the lot build's mask and stamp weight equal the previous header's on a synthetic 40 m lot | CPU harness |
| 2026-10-23 | `40f351e` | `lot_uv_fixture.cpp` (`run_lot_uv_checks.ps1`), compiled at ps_3_0 as in production, with a non-square lot map check | 130,699 passed, 0 failed | Native D3D9 |
| 2026-10-23 | `40f351e` | `tools/terrain_lighting_test` (`run.ps1`) | 1,303,085 checks, 2 failures: *both captured terrain variants tested* and *GPU reach law matches the walls' law*, which also fail at commits from the 2.15.2 period (known, not from this release); the gated indoor light pass equals the previous shader at gate 0 and the CPU formula at gate 1 | Native D3D9 |
| 2026-10-04 | `2584e01` | Lot UV inversion and shader compilation/device acceptance | 128,512 passed, 0 failed (RTX 4070 Ti SUPER) | Native D3D9 |
| 2026-10-04 | `2584e01` | Resource restoration and UV fallback | 13,682 passed, 0 failed | Native D3D9 |
| 2026-10-04 | `92582b6` | 60 GPU scenarios of the production lot HLSL at night vs the previous HLSL | Within one 8-bit LSB, including soft-edge weights | Native D3D9 |
| 2026-10-04 | `92582b6` | Failure injection of the c28..c31 read | Native draw kept | Native D3D9 |
| 2026-10-27 | branch `perf/lot-streaming` | `tools/crt_math_test`: `Pow2` against `std::ldexp` over every half value; `ApexMath::Hypot` against `std::hypot(float, float)` over 26 million pairs (262 million in a one-off run), random bit patterns included | Same bits for every half and every pair | Offline CPU check |
| 2026-10-27 | branch `perf/lot-streaming` | `tools/lighting_prepare_test` with the production `FenceGapsBuild` (now `ApexMath::Hypot`) against its reference | 36 geometry cases exactly equal | Offline CPU check |
| 2026-10-27 | branch `perf/lot-streaming` | Microbenchmark (x86, /O2 /fp:precise) | `std::hypot` 124.7 ns -> `ApexMath::Hypot` 2.2 ns a call; `std::ldexp` 17.7 ns -> `Pow2` 2.5 ns | Offline CPU check |

Since `0c418a6` the lot UV fixture compiles the replacement at ps_3_0, the profile production uses (`kReplacementPsId`);
at ps_2_0 it had no c32 and failed since the lot shade's mask rows moved there.

## In-game test plan

1. Night, stand at a lot border next to a street lamp. **Expected:** no straight cut between lot and world grass.
2. Developer > Lighting > Surface and provider state, "Street lamps in lots". **Expected:** "Active | terrain chunks
   seen: N | lot light fixed: N draws ... | without terrain texture: M", with M near 0 once the atlas is ready.
3. F7 on lot grass. **Expected:** the covering light pass is the replacement (ps_3_0) with s2 bound to the 2D atlas
   render target (2560x2560 or 4096x4096 in the captures) or a 1024x1024 A8R8G8B8 smoothed map.
4. Toggle *Street lamps light lots* live. **Expected:** the cut returns when off.
5. Soft lot edges: F7 on lot grass 0.2 to 0.5 m inside the edge next to a lamp, then F7 on the world grass just outside.
   **Expected:** the lot pass line reads `mod draw: lot light pass | soft edges: lot <id>, W x D m, origin (x, z), band
   3.0 m (PS c28..c30)` with the real lot size (for example 30 x 30); the lamp term inside matches the terrain term
   outside within about 2 to 3%. Developer > "Soft lot edges" A/B; the status "Soft lot edges: on, band 3.0 m | lots
   known: N | lot passes feathered: M | without a lot rectangle: K" keeps K near 0 once lots are loaded.
6. A closed wall at the lot's outer edge with a lamp inside. **Expected:** no indoor light row on the grass just outside
   the wall (lot UV alignment).
7. Daytime in Build mode with lamps lit. **Expected:** lot and world grass show the same subdued lamp term across the
   border.
8. Lamp shade on the GPU (not checked in game yet). Developer status of *Walls shade lamps on lot ground*: **Expected:**
   *GPU: N lot builds ...* and the log line *Lamp shade on lot ground: built on the GPU (chunk shade L8)*; on a device
   without the formats, *GPU: off (reason)* and the shade as before.
9. Drag a garden post along a free-standing wall, a fence with a gate and a tree, Instant lamp lighting. **Expected:** the
   wall's, fence's and crown's shadows follow the lamp every frame, on the lot and on the world grass past its edge, with
   no seam at the lot's edge; at rest the same picture as Developer > Tester > *Ground shade on the GPU* off (A/B
   screenshots of the same view); frame time while dragging no worse than with the switch off.
10. The same at rest in each variant that reads the shade, switching weather and season mid-game without moving the camera.
    **Expected:** the shade unchanged by each switch, the same as with the switch off:
    - world grass around a lot: smoothed maps on and off (plain and smoothed terrain copies), daylight dusk (daylight
      range copy), multi-pass terrain, dry, wet (light rain), and snow at each level (no snow, light, deep);
    - heavy rain on the terrain variants that use every sampler: no wall shade on the world grass on either path;
    - the lot pass and its snowy and melting passes (lot grass under fresh snow, melting snow);
    - story-0 paths and decks, and snow on floors (`FloorShadeBind`);
    - roads and sidewalks next to a shaded lot, with and without snow on sidewalks;
    - fences, curbs and stairs (instanced groups) within 1 m of the ground.
11. Alt-tab out of full screen, or change the resolution, with shaded lots on screen. **Expected:** every lot's shade back
    within a second (the masks and shades are made again after the reset), no white or black ground.

## Confirmed in game

- 2026-10-30, developer build `70320F96` of `5537a83`: a garden post 1.5 m inside a lot beside a low terrace wall no longer leaves
  a straight cut along the lot's edge; before, the lot's shade was 0.12 just inside and the world grass's 0.55 just
  outside (light captures with both textures; player report).
- 2026-10-29, developer build of `7839649` (2.18.1): a brick fence type removed whole with the camera still lost its
  shadow at once (log: `fence group gone: not drawn for 516 ms`; player report). The same session also dropped groups
  with no walls and one wall of another lot together, most likely hidden by a view change; they are read again when
  drawn.

- 2026-10-29, developer build of `d8501f2`: adding and removing brick fence pieces and dragging a lamp beside them update
  the ground shade without the earlier half-second steps; rare stalls reported before the 100 ms checks (player report).
- 2026-10-29, developer build of `ee3cc46`, F7 on a brick garden wall: its instanced draw's pixel shader (`PS_A2C73090`,
  diffuse 512 x 256 DXT1) has no `texkill`, so `FindCutout` finds no cutout and its walls pass no light, as before the
  mesh fence share. The two 256 x 256 DXT5 cutout textures with an open share of 0.258 in that scene belong to another
  fence's shader.
- Combined build (notes section 1): the lot pass with `max(lot, terrain)` removes the straight cut.
- 2026-10-02, terrain-variant build `C1257AF49C613DF227B5EBAD639D795665D77E378A0FB3744DB948AFA0883B39`: the reported lot/world
  cutoff resolved in the tested scene (observed in game, no new GPU comparison). The earlier lamp-off package
  `D593AECA5885682FEA5D6EDCDAB3F038512CC6B08D8CDC1371A4F456041DD88D` predated the multi-pass correction.

## Open checks

- 2.18.3: right after a world loads, the ground shows the lamps without the walls' shade until each lot's shade is built
  (174 lots in about 50 s in the tested neighbourhood), then dims where walls block a lamp; more visible since the shade
  past a lot's edge is no longer lightened by the lots next door (player report).
- 2.18.3 review: a wall's shadow that crosses the grass onto the ground of a lot next door now reaches that lot's edge
  at full strength, while that lot's own pass tests only its own walls: the step can move to its edge (a wall about 2 m
  from a shared lot edge would show it). A lot's shade built before a lamp was moved or removed shows at full strength
  over the grass until it is built again. Two lots' walls blocking different lamps at one texel give the larger loss,
  not the sum (lighter than true, darker than before).
- 2.18.3: why the shade inside that lot was as low as 0.12 two metres from the post on its lawn side, where no wall
  stands between them, was not found: the captures do not list the walls the shade build saw.
- Mesh fences: a fence that looks solid but is drawn with a cutout shader. **Expected:** its shadow as dark as a wall's.
  The share covers the whole texture, so unused transparent texels around the fence's own area would let light through;
  not seen yet (the brick garden wall has no cutout).
- A fence opening in front of a lamp (brick fence, lamp 1.5 m behind the gap). **Expected:** the lamp's light on the
  ground in front of the gap. An offline build of the shade with the logged pieces gives the beam (194 to 255 in front
  of the gap); a screenshot of the scene before the gap fixes showed that ground as dark as the shadow. Not checked again
  since.
- The cost of the 100 ms shade input checks per drawn lot has not been measured.
- 2.15.3: a fence lit by one garden post on grass and on snow. **Expected:** the shadow behind it ends where the post's light on the ground ends and is never darker than the ground no lamp of the lot reaches.
- 2.15.3: a closed gate in a fence line beside a garden post, after shade updates, after a few minutes on screen and after the camera returns. **Expected:** no light through the gate; gaps under 0.4 m closed; developer log lines for the gaps and gates.
- 2.15.3: a lot's shade after a wall or lamp change. **Expected:** the lot and the world grass beside it change in the same frame (no seam at the lot's edge), the lot nearest the camera first; the developer log's time to show is plausible.
- 2.15.3: a ground lamp on a non-square lot (50 x 50 m with a 512 x 256 map). **Expected:** its pool centred on the lamp, round where it meets the atlas.
- 2.15.3, *Indoor light adds to the ground*: a lit window beside a street lamp's pool, on grass and on snow, the option on and off. **Expected:** the window's light adds to the pool and stays whole behind a wall that shades the outdoor lamps; off, the look of 2.15.2.
- The runtime value of room 0's +0xC0 / +0xC4 has not been printed in a log.
- PR #2: lot UV alignment, daylight terrain term and the c28..c31 read guard in gameplay and under DXVK; whether the
  failed constant read ever occurs in play.
- Winter variants and other untested scenes for the continuity fix.
- Per-pixel lamp term on the lot-map part (`max(lampTerm(lotMap), terrain)`, PASSO3 increment 4): not started.
- Terrain stamp re-rendered at 4 texels/m (increment 5) would sharpen world and lot grass.

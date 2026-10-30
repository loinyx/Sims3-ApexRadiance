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
| [`tools/ground_shade_test`](../../tools/ground_shade_test/check.cpp) | `ground_lamp_shadow.h`: the lot build timed on a synthetic 40 m lot (8 lamps, 20 walls, soft and hard edges, turned and not) and, with `-Reference <revision>`, compared bit for bit with that revision's header; `ChunkUpdate` against `ChunkFull` after random lot changes; the byte rounding against `lround` over the range | `run.ps1 [-Reference <revision>]` | None (CPU) |
| `tools/terrain_lighting_test/resource_paths_fixture.cpp` | UV fallback and resource restoration | `run_resource_checks.ps1` | Native D3D9 GPU |

## Latest results

Release validation for `v2.15.6`: Terrain package layout coverage and exact fence gap reconstruction; unknown layouts retain fallback. The reviewed implementation is `f66e777`;
final release build and gameplay confirmation are tracked with the release tag and PR. Offline results do not establish
an FPS gain, complete climate coverage or resolution of texture allocation stalls.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
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

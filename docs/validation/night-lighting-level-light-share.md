# Light between stories: validation

The feature is described in [features/night-lighting/level-light-share.md](../features/night-lighting/level-light-share.md).
It must:

- Give every story's room 0 the outdoor lamps of every other story, with the weight each lamp has on its own story.
- Never let a borrowed lamp pass a wall of its own story or of a story in between, and never touch the room's own lamps.
- Light an indoor room with a lamp of another story only through an opening in every floor the real ray crosses; fail
  closed when a floor, manager, room or crossing order cannot be read.
- Keep native light range and attenuation; add no artificial brightness.
- Give the rooms of one atrium the same ambient colour and normalisation, and meet the walls above and below a floor line
  on the same light.
- Send only the affected rooms to gather again, with no re-gather loop, and leave rooms whose lamps did not change at
  rest on a floor switch.

## Automated tests

`tools/room_gather_queue_test/run.ps1` reproduces a room invalidated during a gather whose pending key is then erased. It checks restoration without changing the countdown, completion through the native work model, and guards for idle, inactive, indoor and already queued rooms. It uses a CPU model, not the game engine.


| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/exterior_lookup_test`](../../tools/exterior_lookup_test/README.md) | Extracted exterior floor lookup and per-point cache against the v2.10.0 implementation; alternating stories, replacement, missing data and eviction | `run.ps1 -ReferenceSource <v2.10.0-level_light_share.cpp>` | MSVC x86 |
| [`tools/terrain_lighting_test/run_indoor_stories_checks.ps1`](../../tools/terrain_lighting_test/run_indoor_stories_checks.ps1) (`indoor_stories_fixture.cpp`) | Extracts the production `IndoorBoundaryPass`, `IndoorPassImpl` and `BasisLightHook` and runs them on mock game structures: all story pairs, open shafts, every solid intermediate floor, wall blocking, wall-mode restoration, tile heights, unavailable data, ghost rows and split-level crossings, the raised-room exterior wall veto, partial basis transmission. Optional `-ReferenceSource` compiles an older `IndoorPassImpl` to compare adjacent-story results | `run_indoor_stories_checks.ps1 [-OutDir ...] [-ReferenceSource ...]` | MSVC x86 |
| [`tools/room_structure_test/run.ps1`](../../tools/room_structure_test/run.ps1) (`check.cpp`) | Extracts `NoteRoomStructure`: a new room schedules a refresh, LOD or repeated gathers do not, roof and wall changes do, repeated changes coalesce | `run.ps1` | MSVC x86 |
| [`tools/roofed_openings_test/run.ps1`](../../tools/roofed_openings_test/run.ps1) (`check.cpp`) | Extracts the story grid, its ray walk (`GridPassImpl`) and the roofed rooms' doors (`RoofedDoors`, `DoorSig`, `DoorSeen`) and runs them on synthetic grids: a roofed room with a two-cell arch, a lamp in front sees it, one behind or beside does not; the lit floor patch through the arch is bounded by the jambs with no seam between the cells; a ray over the wall's top meets the room's roof; a closed room has no door and stays dark; a window's sill; an arch into another roofed room is no door, into a roofless room it is; the signature follows the doors | `run.ps1` | MSVC x86 |
| [`tools/room_ambient_test`](../../tools/room_ambient_test/README.md) (`room_ambient_test.cpp`) | Room ambient and scheduling policy (`room_ambient_policy.h`): group deadlines, first dispatch, overdue updates, tick wrap, window recheck timing (0 / 2 s / 6 s, bounded, wrap), lamp refresh delay (120 / 200 ms), safety-net gap (2 s / 500 ms), edit urgency (8 s cap) and edit hold decision, gather ordering, floor priority, structure and floor-edit timing | `run.ps1` | MSVC x86 |
| `tools/mesh_occlusion_test/` | The retired mesh blockers' collector, BVH and replacement policy, in the standard and development configurations | `run.ps1 -OpaqueShader <ps.bin> -CutoutShader <ps.bin> -Standard` or `-Development` | MSVC x86, two captured pixel shaders |
| `tools/liveness_prune_test/` | `liveness_prune.h`: an entry survives one miss and is erased after two sweeps in a row without its key; key 0 is never live | `run.ps1` | MSVC x86 |
| [`tools/basis_test/basis_test.cpp`](../../tools/basis_test/basis_test.cpp) | `PatchBasisSmooth` / `PatchIndoorBasis` on a captured pixel shader, disassembled with d3dcompiler_47 | `basis_test <ps.bin> [lmSampler]` | d3dcompiler_47, a captured shader |

These harnesses do not validate the game's room topology, lamp falloff, visual convergence or performance. They read
only the repository and print to the console.

## Latest results

Release validation for `v2.15.6`: Room queue policy and native reader/insertion fixtures; exact lamp RGB and wall blur comparisons. The reviewed implementation is `f66e777`;
final release build and gameplay confirmation are tracked with the release tag and PR. Offline results do not establish
an FPS gain, complete climate coverage or resolution of texture allocation stalls.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-27 | `5006c39` (kept solves account for every light) | `tools/outdoor_delta_test`; `tools/development/check.ps1 -Suite Smoke`; dev build in game | 0 failures. In game: 21 kept room-0 solves over lamp recolours, moves and switches, none with light past its parts (the check never refused); the stale courtyard light of 2.16.0 not reproduced. Its cause is not confirmed | DXVK 3.1.1 in game |
| 2026-10-27 | `47ec493` (release 2.16.0 candidate; the cut scaled for opening entries `75f1f5b`, wall lamps `156f092`) | `tools/roofed_openings_test`; `tools/development/check.ps1 -Suite Smoke` and `-Suite Lighting` (with the reference law of `render_cost_test`) | 0 failures. In game (F7/F8 sessions): the living room behind the courtyard window lit from 1.5 m to the back wall (9 down to 4 levels), the sill's shade before it, as computed; wall lamps not yet seen in game | Native D3D9 (offline device) |
| 2026-10-26 | `8b59910` (the outdoor texel's cut at an opening) | `tools/roofed_openings_test` (1,724 and 175 checks; `OpeningShareChecks`); `tools/outdoor_delta_test` (15); `tools/rt_lamps_test` (23); `check.ps1 -Suite Smoke` and `-Suite Lighting`; developer, public and standard Release x86 builds | Passed, 0 failures. The green sconce's replay at the hall's glass door: 0.640 before, 0.207 now (its part of the courtyard's texel there 0.207); two lamps at 1.2 each: the room at the door 2.00 before, 1.00 now. Not checked in game | CPU harness / native D3D9 |
| 2026-10-26 | Working tree on `d542127` (outdoor texels keep their hue) | `tools/outdoor_delta_test` (15 checks); `tools/rt_lamps_test` before its texels-past-1 count was added (22 checks: the compose shader equal to the pack on 26,214 texels); `check.ps1 -Suite Smoke` and `-Suite Lighting`; `tools/roofed_openings_test` (1,703 and 175 checks); `tools/floor_shade_check` (9,624 checks); `tools/terrain_lighting_test` on the F7 13-00-02; developer, public and standard Release x86 builds | Passed, 0 failures, except the terrain test's two known failures; the courtyard's case packs (255, 38, 15) instead of (255, 255, 105). The added count of texels past 1 in `rt_lamps_test` did not run: Windows refused the rebuilt test program. Not checked in game | CPU harness / native D3D9 |
| — | `v2.15.6` (implementation `f66e777`) | `room_gather_queue_test; render_cost_test; lighting_prepare_test` | 648 policies; 2048 native set comparisons; 131072 lamp RGB comparisons; 144 wall blur cases passed | CPU fixtures; native layouts |
| — | `10c8107`; `v2.15.5`, ASI SHA-256 `24A2A83A2B189E7FA5CF88B847E5E9B1F68C36E6428B9FCC744E76735A6E1DAB` | Standard Release x86 startup; i18n check; menu audit | No build warnings; all 21 languages cover the new line; embedded cache supplies 78 shaders with 0 to compile; developer mode off and temporary script probe removed. The exact standard build returns normally from Build/Buy to Edit Town | Steam / DXVK |
| — | `5dd19f1`; release `v2.15.5` | Room gather queue regression; CPU Lighting suite | Lost-key reproduction and queue guards pass; 92 lighting checks pass. Gameplay reproduction returns to Edit Town with the correction | CPU model / Steam gameplay |
| 2026-10-23 | `4802ed9` (ASI SHA-256 `FC2D565C0F003F0DD9DF2BC56DFC5F047CA6FCDAC508B2DA66A83D032258ECFB`) | Standard Release x86 build; `tools/roofed_openings_test`; `tools/floor_shade_check`; `tools/ground_shade_test/run.ps1` | Build: 0 warnings. Roofed openings: 1,652 checks, 0 failures; floor shade: 7,885, 0 failures; ground shade: 240, 0 failures. The exact release build has not been checked in game | MSBuild x86 / CPU fixtures / native D3D9 |
| 2026-10-23 | `40f351e` | `tools/development/check.ps1 -Suite Smoke` and `-Suite Lighting` (includes `tools/roofed_openings_test`) | Passed, 0 failures. `roofed_openings_test`: 217 checks, 0 failures (window sizes from their panes, the wall's two faces at grazing angles, the glass share, floor-level sills in the outside walls' copy). No harness covers the lamps under a story's floor (`OwnFloorBlocksAt`, `LampUnderRoomFloor`), the tile floor crossing or the walls lit from their drawn foot | CPU harness |
| 2026-10-22 | `d625404` | `tools/development/check.ps1 -Suite Smoke` and `-Suite Lighting` (`stairwell_bounce_test`, `opening_bounce_test` 11 checks, `arch_ambient_test` 25 checks), `tools/rt_lamps_test` (22 checks), `tools/roofed_openings_test` (196 checks), `tools/room_structure_test` (11 checks); developer, public and standard Release x86 builds | Passed, 0 failures; the three builds compiled. These cover the extracted grid walk, openings and room structure code; none exercises the arch story memo of `823e339` (`ArchAmbientConsts`) | CPU harness |
| 2026-10-22 | `7d69737` | `tools/development/check.ps1 -Suite Lighting` (`stairwell_bounce_test`, `opening_bounce_test`, `arch_ambient_test`; `stairwell_bounce.h` now uses `ApexMath`) | Passed | Offline CPU harnesses |
| 2026-10-22 | `715a410` | `tools/development/check.ps1 -Suite Lighting` | Passed | Offline CPU harnesses |
| 2026-10-22 | `fd85116` | `tools/development/fast_math_check.cpp`: `ApexMath::Sqrt`, `Fabs`, `Floor`, `Ceil`, `FloorToInt`, `CeilToInt`, `IsFinite`, `IsNaN` against the CRT over all 2^32 floats (x86, /O2 /fp:precise) | Same bits for every input; a signalling NaN input may come back unquieted from `Fabs` and `Floor` | Offline CPU check |
| 2026-10-22 | `fd85116` | Early rejection of `SegPass` / `PassesOverLowerWall` against the original t test, 180 million random and special cases | No difference (an infinite denominator takes the original path) | Offline CPU check |
| 2026-10-22 | `fd85116` | `check.ps1 -Suite Lighting` and `-Suite Smoke`, `rt_lamps_test`, `stairwell_bounce_test`, `roofed_openings_test` (the extracted grid walk with the near-edge map), `room_structure_test` | Passed | Offline CPU harnesses |
| 2026-10-22 | `715a410` | `tools/rt_lamps_test/run.ps1`, `tools/stairwell_bounce_test/run.ps1`, `tools/roofed_openings_test/run.ps1` | Passed: 22, 56 and 196 checks, no failure | Offline CPU harnesses |
| 2026-10-22 | `522ef32` | Exhaustive comparison of `ApexMath::Sqrt` / `ApexMath::Fabs` (`features/fast_math.h`) with `std::sqrt(float)` / `std::fabs(float)` over all 2^32 float bit patterns | Bit-identical for every input except signalling NaNs, which `std::fabs` returns quieted and `ApexMath::Fabs` unquieted (rerun with `tools/development/fast_math_check.cpp`; no arithmetic produces a signalling NaN) | Offline CPU check |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/mesh_occlusion_test/` | Standard: 20,017 checks passed; development: 20,119 checks passed | None |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/liveness_prune_test/` | 27 checks passed | None (pure) |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/roofed_openings_test/` | 196 checks passed, including the stairwell veto, floor-level sill, hard wall top and arch cone cases | None (pure) |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/room_structure_test/` | 11 checks passed | None |

The mesh blockers are no longer tested by shipped indoor light; the results below describe the collector itself.

2026-10-19, public test 11 candidate: a read-only test 10 snapshot contains 23 groups and 8186 triangles, mainly narrow
fence/post batches. The earlier captured stair draw has 136 primitives and eight instances (1088 triangles), which
cannot fit in the remaining six slots. This demonstrates a coverage limit; it does not prove that every missing group
was rejected only by that limit or that the scene remained unchanged between observations.
The mesh fixture reproduces 8186 occupied slots and admits a 136-primitive, eight-instance, 28-byte-layout synthetic
stair batch by replacing lower-coverage groups. The resulting scene has 7850 triangles; solid treads block and open
gaps remain clear. It also checks no partial removals on a failed plan, oversized/higher-priority protection,
group-cap replacement, buffer deduplication and the previous ray/BVH tests: 20,117 checks pass. Captured shaders are
used with synthetic buffers; actual mesh coverage, visual convergence and runtime cost remain unverified.

2026-10-19, public test 9 candidate: test 8 captures still show the coloured upper-wall patch. Read-only inspection
of its collector finds four 380-triangle groups outside the stair area, rather than the stair instances recorded by
the probe. Presence of a published BVH is therefore not evidence that the staircase was collected. The candidate
removes width and dynamic-usage exclusions and tests narrow/dynamic geometry with eight rotated instances using the
captured 28-byte FLOAT3/FLOAT4 instance layout. Identical geometry in distinct transient buffers does not consume
additional scene groups or triangle budget. The mesh fixture passes 20,100 checks. The fixture retains synthetic vertex/index data; actual scene coverage
still requires the accepted-group log and in-game validation.

The indoor fixture also covers blur support rows below/above a closed slab: imported light is rejected in both
directions, while true openings keep their prior transmission (19,209 indoor/basis checks pass). This closes a separate path by which support rows
could seed light into an otherwise blocked visible wall. Ordinary ghost/shaft and native wall/floor fixtures remain.
Correcting these paths does not establish complete visual convergence of the reported arch or every stair asset.

2026-10-19, public test 8 candidate: `tools/mesh_occlusion_test` passes 20,067 checks. The extracted production
collector runs against native D3D9 buffers, captured opaque/cutout shaders, both index formats, nonzero base/minimum
vertices, closed treads and open gaps. Rejected early thin geometry yields later frames to subsequent meshes.
The BVH agrees with a triangle sweep over 20,000 rays; endpoint self-intersection and concurrent scene replacement
are covered. Vertex/index data in this fixture is synthetic; the captured shaders do not establish actual scene
geometry coverage.

The refreshed indoor fixture extracts the complete production basis hook and passes 19,205 checks. Mesh blockers
reject both ordinary cross-story rays and their directional contribution; removing the mesh restores the opening,
while a native slab still blocks it. Opening shares and source normalisation above/below one preserve previous
lamp contributions. Native game functions and room metadata remain mocked.

Read-only scene inspection confirms source-room normalisation 1.5 and recipient normalisation 1.0 beside an arch.
The copied native opening grid admits rays to both sampled floor positions, so a closed arch classification is not
established as the cause of the visible cut. Room ambient colours also differ. The directional-weight inconsistency
is corrected in code; visual convergence, wall-filter leakage, actual tread collection and gameplay cost remain
unverified. No reduction of RAM or additional FPS gain is established by these checks.

2026-10-18, public test candidate: the indoor fixture is refreshed for the current entry, lookup and timing helpers.
It extracts the imported-indoor branch of BasisLightHook; unrelated opening/daytime branches are excluded from this
fixture. Before the guard change, four added checks fail: omitted source/recipient wall tests, adjacent missing-room
fallback, and repeated lamp-position reads. Afterward 30,897 checks pass, including the prior implementation's adjacent
wall/ghost sample results, closed intermediate floors, open shafts, partial transmission and context restoration.
Scene captures identify a red kitchen lamp imported into upper rooms, but do not establish which guard caused the
visible patch. Visual correction in that scene remains unverified; the fixture is not an in-game ray trace.

Release preparation: the public x86 build passes with zero warnings. Exact release commit identity is recorded in the release PR; final artifact gameplay and menu-footer validation remain open. Gameplay evidence applies to the final development candidate ASI (`26CAA132F3D4DD6921E7E3CFFA8BEFA09B3F73868AEA3A917B92B7873D1D8C0D` SHA-256), with wall updates, fence lighting and gate faces visually accepted. This does not establish general FPS gains or exhaustive scene coverage.


| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| — | `10c8107`; `v2.15.5`, ASI SHA-256 `24A2A83A2B189E7FA5CF88B847E5E9B1F68C36E6428B9FCC744E76735A6E1DAB` | Standard Release x86 startup; i18n check; menu audit | No build warnings; all 21 languages cover the new line; embedded cache supplies 78 shaders with 0 to compile; developer mode off and temporary script probe removed. The exact standard build returns normally from Build/Buy to Edit Town | Steam / DXVK |
| 2026-10-14 | `release/2.13.0` (`2aefeb7`) | Developer and public Release x86 builds; `tools/i18n_check`; `tools/menu_audit.pl`; `tools/roofed_openings_test`; `tools/rt_lamps_test`; `tools/outdoor_delta_test`; `tools/floor_shade_check -Precomp`; `tools/room_structure_test`; `tools/room_ambient_test` | Both builds: 0 errors. i18n_check OK (2118 keys, 17 tables complete). Roofed openings 173, real-time lamps 22, outdoor delta 11, floor shade 5842, room structure 11 and room ambient 107 checks, 0 failures; the unlit-rooms recovery executable of `room_ambient_test` was removed by security software after it compiled and did not run | native D3D9 |
| 2026-10-13 | `release/2.12.4-arriscada`, outdoor light through openings | Developer and public Release x86 builds; `tools/roofed_openings_test`; `run_resource_checks.ps1`; `run_lamp_state_checks.ps1`; `tools/room_structure_test`; `tools/i18n_check` | Builds: 0 errors, 0 warnings; roofed openings 18 checks, 0 failures (it failed twice before the jamb fix: the seam between two arch cells); resource paths 14,618 checks, 0 failures; room structure 11 checks, 0 failures; lamp state passed; i18n 21 languages complete. The indoor-stories, world-pool, queue, dispatch and exterior-lookup harnesses still do not compile against the current source (as before) | None (mocks) / x86 build |
| 2026-10-13 | Release preparation tree for 2.12.3; final SHA in the release PR | Developer and public Release x86 builds: 0 errors, 0 warnings; `tools/room_ambient_test`; `run_resource_checks.ps1` | Builds passed; room ambient policy 102 checks, 0 failures; resource paths 14,154 checks, 0 failures (*Buildings block light on objects* 14). The extracted indoor-stories, world-pool, queue and dispatch harnesses no longer compile against the current source (the same failure at v2.12.2), so they were not run | None (mocks) / x86 build |
| 2026-10-12 | Release preparation tree; final SHA in PR | Public Release x86 build; Wall generation policy fixtures | Zero build warnings; Passed the clean source policy checks; no FPS or instantaneous-update guarantee | Native D3D9 fixtures / x86 build |
| 2026-10-07 | v2.10.1 | Extracted exterior floor lookup against v2.10.0 | 86,364 checks passed; 84,000 to 7,000 full link searches in the fixed multi-story fixture | None (native accesses mocked) |
| 2026-10-05 | PR #2 (`2584e01`) | Indoor stories fixture, raised-room wall veto | 18,992 extracted checks passed; the raised-wall fixture failed twice before the guard and passes after | None (mocks) |
| 2026-10-04 | PR #2, candidate `2.5.6-indoor-wall-guard-test` | Extracted indoor and basis tests | 30,890 passed | None |
| 2026-10-04 | same | Resource restoration and lot UV fallback tests | 13,682 passed | Native D3D9 |
| 2026-10-04 | same | Lot UV inversion and production ps_2_0 compilation and device acceptance | 128,512 passed | Native D3D9, RTX 4070 Ti SUPER |
| 2026-10-04 | PR #2, `2.5.6-indoor-stories-test2` | Indoor stories fixture | 6 regression assertions fail on test1 and pass on test2; adjacent-story results match the previous implementation | None |
| 2026-10-01 | `2.5.2-test005` | Group, startup, priority and cache checks with the extracted group publisher and cache forgetting function | 36 passed, plus 89 policy checks; extracted pending loop, stale-lamp visitor and Rooms at Night checks passed | None |
| 2026-10-01 | `2.5.2-test004` | Policy checks; extracted pending loop (4,500 ready entries behind 500 busy, cursor removal, rehash, empty queue); extracted stale-lamp visitor | 89 policy checks and both extracted harnesses passed | None |
| 2026-10-01 | `2.5.2-test001` | Policy checks (short and drag debounce, gather ordering, tick wrap) | 65 passed | None |
| 2026-10-01 | `2.5.2-window-sync-test` | Window recheck scheduling (delays, bounded completion, tick wrap); `LightEntryUpdate` signatures on the installed TS3W.exe | 58 checks passed; one match per signature, both resolving to `0x006C7BA0` (229 address ids, 0 mismatches) | None |
| 2026-10-01 | `2.5.2-room-sync-live-test` | Live group background (delta preservation, round trips, weighted merge, normalisation, zero brightness) | 51 passed | None |
| 2026-09-30 | combined build | Directional-map shader cap | 4 captured indoor-object shaders patched and valid | d3dcompiler |

## In-game test plan

### Exterior floor lookup (2026-10-07)

Source base: tag `v2.10.0`, commit `676889b5b8567e791cb6655b3af62d0f1cadc7e2`; released change: v2.10.1.
The exact reviewed/tested release SHA is recorded in the release PR and local artifact manifest.

`tools/exterior_lookup_test/run.ps1 -ReferenceSource <pre-change level_light_share.cpp>` extracts the production
lower-wall floor lookup and existing `LevelForPoint`, comparing them with the pre-change lookup. Result: **86,364 checks,
0 failures**. It covers alternating floors/lamps, the single-floor warm shortcut, manager-address reuse with the old
floor released, missing floors/managers, recovery, slot eviction across lots, next-point expiry and the existing
read-only self-check. A fixed multi-story mock workload uses 7,000 full link searches instead of 84,000; this is an
operation count, **not a measured gameplay speedup**. Geometry, shader output and end-to-end presentation latency are
not validated by this fixture.

No colour, intensity, falloff, occlusion, floor/wall geometry or rendering policy is changed. Source comparison against
v2.10.0 shows only the `LevelFor` to `LevelForPoint` substitution and comments in the lighting module. The release also
updates its version and translated What's new text. The earlier priority experiment is excluded; see
[history](../history/night-lighting-level-light-share.md).

**Confirmed in game:** the lookup candidate gave substantially faster exterior lighting
updates in the reported Build/Buy scene, and the result was accepted for release. This is qualitative gameplay evidence;
no millisecond, frame-time or FPS improvement is claimed.

**Open checks:** quantitative end-to-end timing and broader lots/backends; pixel-by-pixel visual equivalence on all
scenes. The per-point cache assumes topology does not change inside a synchronous point evaluation, as do the existing
lamp-position/room/floor caches. Manager replacement and recovery between points are covered by the fixture; concurrent
topology mutation inside a point is not reproduced. This release does not promise instant updates on every lot.

### Existing scenarios

1. Two-story house at night with a wall sconce on the upper outside wall near a corner and garden lamps. **Expected:** the
   wall below the sconce is lit continuously across the floor line; the lower side wall around the corner is not
   brighter than the upper one.
2. Developer mode, Developer page status *Stories*. **Expected:** `Active | outdoor lights carried to other stories: N |
   stories updated: N | walls of the light's story: 9/9 classes, N tests, N blocked` (plus *on another thread* or
   *failures* when non-zero). Log: `[LevelLightShare] Installed (walls of the light's story: 9 of 9 classes; ...)`.
3. F8 after 10 s with a still camera. **Expected:** the per-story room 0 lists hold the same lamps on every story,
   level-0 lamps twice; samples near each light show the game's wall test and Apex's factor.
4. F7 on both walls of the floor line. **Expected:** the light map in `s2` of both stories shows the lamp.
5. A red lamp next to a stairwell on the upper story. **Expected:** the wall of the story below is lit red where the lamp
   sees it through the opening, and nothing is lit through solid floor. F8 *STORIES INDOORS* lists a floor object per
   story, openings on the upper story and a gather line for the lower room. Turning *Indoor light between floors* off and
   on brings the light back within about a second.
6. Three-story open shaft with a lamp on the top story. **Expected:** the bottom story is lit through both openings;
   closing either floor blocks it.
7. A raised room (tower) on an intermediate story with a lamp on the story above, and furniture below behind its
   exterior wall. **Expected:** no lamp colour on the furniture behind the wall (F7 basis texel of the furniture shows no
   lamp colour).
8. An atrium wall next to a sconce, both stories in view, *Seamless walls between floors* on. **Expected:** no step at
   the floor line, except where a real floor strip separates the walls. The status reports moved samples and zero pieces
   left as the game has them.
9. Start a recording, switch the atrium lamps, save. **Expected:** `Wall seams.csv` holds rows of both rooms at the same
   positions and normals.
10. Enter a lot with openings at night, leave, enter again; restart the game and load the save. **Expected:** rooms are
    right without moving a lamp or pressing *Refresh the lighting*.
11. Switch floors several times with no lamp change. **Expected:** rooms keep their light; the recorder line *Rooms keep
    their light* shows marks kept.
12. Switch a lamp, drag a lamp, remove a floor tile, add a wall that closes a room, toggle a roof. **Expected:** the
    affected rooms on every story update within a second or two and no room keeps re-solving.
13. Repeat scenarios 1 and 5 with Sims3SettingsSetter Lighting Quality at 16 and 32 samples. **Expected:** same light;
    note the cost.

## Confirmed in game

- 2026-10-23, development build of this branch (`bb67164` to `6c38bfb`): ceiling lamps of the story below no longer light the closed room on the story above.
- Outdoor sharing (parts 1 and 2) and the wall test of the lamp's story (part 3), combined build: confirmed in game
  as almost seamless after the first round and much better after the wall test.
- Floor pairing through the story's own floor object (build `e3fab9d4`): the walls above a double-height room lit at once.
- Seamless walls, first version: a very slight crease remained next to a sconce just under the line; the ghost-row blur
  addresses it.
- 2.5.3: Rooms at Night and connected-room behaviour confirmed in game tests (scenario-specific).
- PR #2, indoor seam candidate: the visual result was confirmed in game before the wall guard candidate.
- Session `2026-10-04 16-58-09`, recording `18-11-40` (`2.5.6-indoor-stories-test2`): story 0 room 23, story 1 room 19
  and story 2 room 20 converge to identical ambient RGB after lamp edits; queued at 18:11:30.743, matching ambient at
  18:11:31.399. This measures queue-to-ambient convergence, not visible latency.
- Session `2026-10-01 15-26-42`: rooms 23/19/3/20 of lot `27D24720` change their merged ambient during a slider drag.

## Open checks

- Windows counted by the window built (`ApplyCutPost`, `KeepBuiltWindows`, `UnlitWindows`): a house with windows next
  to an outdoor lamp at night, developer mode. Rotate the camera round the house and stop on each side. **Expected:** the
  rooms' light stays the same; the `[RoofedLamps]` gathers of those rooms keep the same number of doors to the outside
  and no `[Openings] ... windows 6 -> 3` change follows a turn (the line counts window closes marked). Delete a window:
  its light through the wall goes within about a second; undo, or place it on another wall cell: the light follows.
  Offline: `tools/roofed_openings_test`.

- Outdoor lamp edits reach the rooms that take the lamp through their openings (`SendRoom0DependentsTry` with
  `SameStoryRoofed`, `RoofedSendWanted`), Lamp lighting Instant and Classic: the courtyard save, a wall sconce next to the
  kitchen's glass doors. Change its colour in Buy mode, switch it off and on, move it along the wall, delete it.
  **Expected:** the kitchen and the rooms behind the glass doors follow within a second each time, with no story change;
  the log shows a `[RoofedLamps] Room solve` of those rooms right after the edit; rooms that never saw the sconce are
  not solved again. An indoor lamp of the kitchen recoloured still changes the courtyard floor in front of the doors.
  Offline: `tools/roofed_openings_test` (the selection, and that the edit's tail sends the roofed rooms).
- Outdoor lamps through openings at the outdoor scale, and cut closes held 2 s (`opening_lamp_balance.h`,
  `cut_close_hold.h`): the player's courtyard save with the green sconce, at night, developer mode. **Expected:** the
  kitchen and the rooms behind the glass doors are no brighter than the courtyard floor at the same distance from the
  sconce, still lit in green near the doors; turning the camera round the house leaves the rooms' light the same; the
  `[Openings]` lines count closes reopened within the hold and no `windows 3 -> 0` change follows a camera turn; moving
  *Ground brightness*, *Lot lamp brightness* or the walls' *Brightness* changes the light through the openings once the
  slider is let go; indoor lamps, rooms with no opening and daylight look as before. Offline: `tools/roofed_openings_test`.

- Broader lots and other game versions remain unverified. The exact standard build and the development build both complete the reproduced Steam transition; this does not establish every cause of a loading hang.


- 2.15.3: windows sized from their panes. **Expected:** the developer log lists the windows sized from their panes with plausible sizes, and the light through a window onto the floor outside matches the glass; glass passes 0.765 of a lamp's light.
- 2.15.3: gables, a garage's eave band beside a house on a foundation and sloped foundation faces at night. **Expected:** no hard step of light where a gable starts or under the eave; lamps that now test lower samples keep their light.
- Window light channel memo (`7d69737`), in game: on a lot with window lights at night, F8's *Window lights' channel
  test* shows the answers kept for the sample batch well above those asked of the game, and the rooms look the same
  with the Tester switch *Window light channel memo* off and on.
- Room solve with `ApexMath` and the 64-slot lamp position cache (`522ef32`): room solve time in the Frame Profiler
  against the previous build on the same save; the light is expected to be identical.
- Real-time outdoor lamps from the shader cache (`0405517`): with Lamp lighting on Instant, the log shows `Taking the
  shadow and relight shaders from the shader cache` and the option becomes active without the former start-up compile;
  no `does not compile` line.
- The second solve of a room (fill or normalisation resend) is urgent only while a lamp edit is pending; after a load it queues as before. Expected: a placed lamp shows its final light without the 1-2 s step, and no 15-21 ms frames in the first seconds after loading a lot with stairwells and arches.
- 2.14.0: indoor light through openings and between stories with the mesh blockers retired: no periodic room
  invalidations in a recording while the camera moves, and no hard tread shadow under stairs.
- 2.14.0, Instant (GPU): place or move an outdoor lamp on a multi-story house. **Expected:** every story takes its
  light, including stories whose light list did not hold the new lamp yet ([rt-outdoor-lamps.md](../features/night-lighting/rt-outdoor-lamps.md)).
- 2.14.0: the lamp-visibility memo and the liveness pruning in a long session (lots streaming in and out, world
  changes), with no change of light.
- 2.13.0 build, in game: outdoor light through openings (lot `B1280029950F0CA0`, story 0, the colonnade's room 48
  and the garden post in front of its arches), developer mode:
  - the log has `[RoofedLamps] Room solve: story 0 room 48: N outdoor lights through its openings: L...` with the post
    among them (F8: room 48 in *outdoor light through openings*); with no line for room 48, the developer line *no door
    to the outside known* tells whether the arches were noted on its outline;
  - a red test lamp in front of an arch makes a red patch on the floor and the back wall inside, bounded by the jambs,
    and nothing behind the solid wall beside the arch;
  - F7 on a column of room 48 (`DescribeDraw`) lists the post among *outdoor lamps through the room's openings* and the
    column is lit on its side toward the arch only;
  - by day the inside takes the day factor (no lit patch at noon); switching the post off removes the patch at once;
  - with *Outdoor light through openings* off everything is as before (the room's list holds its own lamps only);
  - status *outdoor light through openings*: points tested and lit grow, no room is sent again and again (rooms sent for
    lamp edits stays still while nothing changes).

- 2.13.0 build, in game: *Doors block light*: a house with a front door, windows and an arch, a garden post in front of each, in developer mode. **Expected:** with the option on the room behind the door takes no outdoor lamp through it (`[RoofedLamps]` counts no door for a room whose only openings are doors), the ground and the objects behind the door stay dark, the window and the arch still let light in; `[YardGrid]` counts the doors as closed for lamps and F8 prints *doors block light: on*. Switching it off logs `[Openings] Doors block light off: N stories ...` with N the stories with doors, those rooms light through the doors again within a few seconds, and no other lot is lit again. Offline: `tools/roofed_openings_test` (door closed, arch and window open, unknown kind closed, option off as before).
- 2.13.0 build, in game: roofed rooms on a foundation: lot `B1280029950F0CA0`, the garden post inside the colonnade. **Expected:** `[RoofedLamps]` for story 1 rooms 4, 7, 11 and 16 lists the post as a lamp of story 0 below, taken; the columns and the walls inside the arches take its light on the side toward the arch; the floor on the foundation stays dark where the bulb is under its plane; a post under a balcony of story 1 does not light the room above it.
- 2.13.0 build, in game: openings capture: load the save, go to lot `B1280029950F0CA0` (streamed in after the world) with *Light through doors and windows* on, in developer mode. **Expected:** `[YardGrid] lot B1280029950F0CA0 story 0: ... N openings (0 noted cuts with no wall)` with N > 0 and "cuts looked up by its owner lot"; F8 lists that lot's owner id equal to its post lot id with the cut cells found; the yards of lots loaded with the world log the same openings as before.
- 2.12.3 public build, not yet tested in game: a balcony or deck with a lamp under it and walls, windows, objects and fences on it; a lamp on the balcony; a lamp seen past the slab's edge. **Expected:** nothing above the slab takes the lamp below it; the edge dims rather than cuts; the lamp on the balcony still lights the wall below it.
- 2.13.0 build, in game: Lamp lighting on *Instant (GPU)* and on *Classic*, each with a lamp switched, recoloured and dragged on a multi-story lot. **Expected:** Instant updates every floor within a frame or two; Classic relights a dragged lamp after its release and shows the floors together; switching modes relights the lot once; loading another save or travelling with relit maps leaves no stale light and no error in the log.
- 2.13.0 build, in game: peak private bytes below 2 GB with Instant on a large multi-story lot (relit map buffers and the kept room parts, ceiling 160 MB).
- 2.13.0 build, in game: a lamp that keeps moving for more than 20 s with Lamp lighting on Classic. **Expected:** room solves resume after 20 s; `[DragLite]` shows no lamp steps outside Build/Buy mode.
- The indoor-stories, world-pool, queue and dispatch harnesses need their extractions updated to the current source.
- Exact release artifact: final SHA, build verification and unchanged visual behavior after cleanup
- Version footer and unread-version indicator on the final release build; all 21 changelog language tables pass offline validation
- Wider fence/gate materials, fallback groups and daytime transitions


- Both leak paths fixed in the wall guard candidate (furniture behind walls through the directional maps, and the lot
  UV contraction on the ground) and the raised-room veto: gameplay appearance, DXVK and frame-time cost.
- The three-story open-shaft scene with multi-story openings.
- Visible seam between atrium walls where the ambient already agrees (the indoor wall inconsistency reported on test2);
  compare `Wall seams.csv` rows at identical positions and normals, remembering raw samples precede the blur and upload.
- Floor-edit refresh after 250 ms and structure-change refresh: response in game.
- Window activation recheck: whether a stale roof/topology classification needs more than the 0 / 2 / 6 s re-evaluation.
- Whether the floor objects of a lot rebuilt without floor calls are always seen (`LevelCtorHook` covers construction;
  unverified that the game rebuilds them that way).
- Cost of the per-point hooks in game, alone and with Sims3SettingsSetter Lighting Quality at high sample counts.
- An outdoor room 0 of story 3 sent to solve about every 50 ms for 3 s and never finishing (one F8, 2026-09-29): the
  journal fields `Q`/`H`, lot, camera story and flag `+0x19` should identify the source.
- Basements: untouched by design, no report yet.
- Openings in the outside walls' copies (2.12.4): a garden post inside an arcade lighting the grass, the columns and the
  walls outside through the arches; a post outside a door lighting the floor and objects inside; half walls letting
  light over them; F8's *outside walls' openings* lines and the `[WallCopy]` log; the cost of room 0's retest on a lot
  with many openings. Offline: `tools/roofed_openings_test` (52 checks).
- A colonnade on a foundation with a post on the terrain in front of its arches: its floor, columns and back wall lit
  through the arches (offline: the foundation checks of `tools/roofed_openings_test`, 82 checks).
- Indoor light through openings (not checked in game): a lit room's light through an arch onto the terrace and the garden,
  not through a closed door or a wall; into a roofed room next door through an arch, not two rooms away; the outdoor objects
  near the arch lit, those behind a wall not; the `[InnerLamps]` lines and the status counters; cost on a large house
  (offline: `tools/roofed_openings_test`, 99 checks).
- A lamp beside a window (not checked in game): the balcony floor outside as bright as the room's floor at the same
  distance from the lamp, the strip under the sill dark; the status *points scaled by the source room's normalisation*;
  the columns inside an arcade no brighter than those facing the post (*by the per-pixel lamps' law* in the objects status).
- A lamp dragged and put down next to an opening (not checked in game): the next room lit again after every release, the
  `[InnerLamps] ... gathers again` lines; a lamp deleted costs its neighbours four gathers at most (offline:
  `tools/roofed_openings_test`, the retry checks).
- Penumbra (not checked in game): the light through an arch into the next room sharp at the jambs and softer farther
  away, no texel stairs on the floor; *Shadow softness* at 2 cm gives the earlier edges, at 50 cm wide soft edges; no light
  in a closed room's corners; moving the slider lights again only the rooms with openings or yards (the `[Openings] Shadow
  softness` line) (offline: `tools/roofed_openings_test`, 115 checks).
- Openings on lots streamed in late: the `[YardGrid] ... rebuilt with N wall edges and M openings` line for
  `B1280029950F0CA0` and a door found for its roofed rooms; the cuts looked up under the story manager's id on
  `7975270033346670`.

An isolated indoor cross-story update delay in the 2026-10-12 12:57:36 session resolved without a code change. The indoor feature was enabled and installed; no specific rejection cause was confirmed. Retained for follow-up.

- Soft ground shade (not checked in game): the shade of a free-standing wall or a wall opening on lot grass and paths ends
  in a soft edge, wider farther from the wall, with no texel steps; no light behind a wall's joints or corners (offline:
  `tools/roofed_openings_test`, the ground soft checks). Developer > Tester > *Soft ground shade* off gives the hard edges.

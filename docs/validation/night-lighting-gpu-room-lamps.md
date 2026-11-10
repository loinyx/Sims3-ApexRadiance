# GPU room lamps: validation

The feature is described in [features/night-lighting/gpu-room-lamps.md](../features/night-lighting/gpu-room-lamps.md). It must:

- Draw indoor lamps of the active lot at the game's level: within 2/255 of the game's lamp part on the replayed recordings.
- Show a colour, intensity, switch or move edit with no room solve in the frame of the edit.
- Keep the soft furniture shadows of ceiling lamps: `roomLampTrueShadows` off by default.
- Reach every player with no developer switch on Steam 1.67.2 and EA app 1.69.47, and leave builds whose addresses are not found on the game's room solves.
- Free kept rooms on Instant to Classic, Night Lights off and a world change.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/rt_lamps_test/` | Physical shaders, quads against points bit for bit, penumbra | `run.ps1` | GPU |
| `tools/outdoor_delta_test/` | Kept parts, laws of tubes and area lights, split fill, reflection pacing, baked masks, stair set | `run.ps1` | None |
| `tools/room_ambient_test/` | Ambient retake, room tail | `run.ps1` | None |
| `tools/scene_replay/` | Recorded rooms against the game's solves | `run.ps1` | Recorded scenes |
| `tools/story_maps_test/` | The story map scan over a synthetic manager | `run.ps1` | None |
| `tools/settings_migration_test/` | The one-time move to the new lighting defaults, on built configs and the ground key of configs before 2.18 | `run.ps1` | None |
| `tools/lighting_restart_test/` | Lighting restart, the terminal solve states | `run.ps1` | None |

## Latest results

Rows marked *tree* ran on `1f76e83` with the 3.0.0 working tree; the release commit replaces them when the checks run on it.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-10 | release tree | `rt_lamps_test`, `room_ambient_test` (with `unlit_rooms_recovery_test`), after the rows beyond follow their edge (`GhostKeptPS`) | 680,409 checks, 0 failures (the class-2 kept atlas: the GPU against `FollowGhosts` + `BlurBlock`, largest difference 0/255); 153 + 1,345 checks, 0 failures (7 `FollowGhosts` checks) | MSVC x86; native D3D9 |
| 2026-11-09 | release tree | `outdoor_delta_test`, `room_ambient_test` (with `unlit_rooms_recovery_test`), `rt_lamps_test`, `room_solve_causes_test`, `settings_migration_test`, `scene_target_test`, `story_maps_test`, `render_cost_test` | 287, 134 + 1,343, 680,409 checks and every other suite pass with 0 failures; the 17:18 switch replay: last room +2.14 s (+15.53 s before) | MSVC x86 |
| 2026-11-09 | release tree | `i18n_check`, `public_text_check.py` | 2,497 keys in all 21 languages; no public text finding | MSVC x86 |
| 2026-11-09 | release tree | in game, Steam 1.67.2 under the RTX Remix bridge | confirmed in game: the day/night switch and the 6:00 lamp switch of a neighbouring lot without the earlier pauses; not measured after the fix | DXVK x64 (bridge) |
| 2026-11-06 | tree | `scene_replay`, version 3.0.0 | 95.46% within 2/255 of 2,691,056 lamp texels in 10 recordings; walls 92.8%, floors 96.2%, ceilings 97.0%, objects 97.8%. The capture folder no longer holds its 3 oldest recordings, at 95.59% to 97.40%; each of the 10 left reads as in the run of 13 (95.68% of 3,357,815 texels) | MSVC x86 |
| 2026-11-06 | tree | `rt_lamps_test`, `outdoor_delta_test`, `arch_ambient_test`, `room_map_smooth_check`, `settings_migration_test`, `i18n_check`, version 3.0.0 | 140, 224, 4,153 and 1,845 checks, 0 failures; 30 passed; 2,487 of 2,487 keys in every language | MSVC x86; native D3D9 |
| 2026-11-06 | tree | Standard and `APEX_NO_DEV_TOOLS` builds, version 3.0.0 | Both build | MSVC x86 |
| 2026-11-05 | tree | `scene_replay`, last code | 95.64% within 2/255 of 3,414,671 lamp texels in 12 recordings; walls 93.0%, floors 96.4%, ceilings 97.2%, objects 97.9%; the recording of 19-37-16 goes from 95.49% to 94.38%. The recordings hold no leg shadows at these heights, so the fall is the new shadows showing as differences; a baseline with `--drop 0.5` flags that scene | MSVC x86 |
| 2026-11-05 | tree | `scene_replay`, before the leg gate | 95.72% within 2/255 of 3,526,883 lamp texels in 13 recordings; walls 93.1%, floors 96.7%, ceilings 97.2%, objects 97.9%; misses: norm 2.7%, unknown 1.4%, 3D 0.1%, opening 0.1%; worst room 75.0% of 33,350 texels, story 1 room 2 | MSVC x86 |
| 2026-11-05 | tree | `scene_replay`, before the leg gate, one recording fewer | 95.71% of 3,414,671 lamp texels in 12 recordings; those 12 match the run above, and the capture folder no longer holds its oldest recording | MSVC x86 |
| 2026-11-05 | tree | `rt_lamps_test`, `outdoor_delta_test`, `room_ambient_test` | 137, 217 and 116 checks, 0 failures; `room_ambient_test` not run again after the leg gate and the arch reach (`rt_lamps_test`: 302 glazed spans, the two crossed past the 255th; joined tables, panes, `ToneSoft`, bounce; `outdoor_delta_test`: `DawnChecks`, largest lamp step in a frame 0.040 to 0.003 at dawn and dusk, a ceiling lamp matching the top alone past the gate and away from streaks, bit for bit) | MSVC x86; NVIDIA |
| 2026-11-05 | tree | `lighting_restart_checks`, `unlit_rooms_recovery_test`, `settings_migration_test` | 66 and 1,341 checks, 30 passed, 0 failures; 75 real tomls of older versions parsed and moved | MSVC x86 |
| 2026-11-05 | tree | Offline port of the object move on the 2026-11-05 19-37-16 scene | `Bound` matched all 252 records within 1 cm, 24 with discs; the strict arm rule hit 22 of 72 slant records | Python, no game |
| 2026-11-05 | tree | `story_maps_test`, `scene_budget_test` | 0 failed; passed with `public=1` | MSVC x86 |
| 2026-11-05 | tree | Standard and `APEX_NO_DEV_TOOLS` builds | Both build; the Developer page stays hidden in the public build. Only the standard build was made again after the leg gate and the arch reach | MSVC x86 |
| 2026-11-05 | tree | Shader cache, generated offline | 105 of 105 compiled, 0 failed, 492,168 bytes, embedded in the standard build | MSVC x86 |
| 2026-11-05 | tree | Leg gate, simulation (table below) | At or under 1.5 m over the leg's top, bit for bit as before | Offline |
| 2026-11-05 | tree | Drag cost of the leg gate, 2,720 texels, 1 lamp, 64 legs | No legs 3.324 ms; lamp at 1.2 m 3.523; 2.6 m 3.527; 4 m 3.485: legs walked at 2.6 m cost 0.04 ms over skipped ones | NVIDIA, no game |
| 2026-11-05 | tree | Drag round benchmark, 47 maps, 436,140 points, 3 lamps, synthetic scene | 10.9 to 7.5 ms of GPU, 0 of 436,140 sums different | NVIDIA RTX 4070 Ti SUPER, no game |

`unlit_rooms_recovery_test` and `lighting_restart_checks` run from a copy under a fresh name: Windows can hold the built `.exe` as in use.

The leg gate on a 0.72 m table, the lamp 1 m to the side. A table top's own hardest step at the same lamps is about 0.8.

| Lamp height | Deepest shadow | Hardest step per texel |
|---|---|---|
| 2.2 m | 0.179 | 0.128 |
| 2.6 m | 0.095 | 0.055 |
| 2.8 m | 0.067 | 0.036 |
| 3.6 m | 0.001 | 0 |

## In-game test plan

1. Fresh config, public build, Steam 1.67.2, a lot with several stories. Switch, recolour and dim an indoor lamp. **Expected:**
   the room changes in the frame of the edit and no room solve shows in the log until the settle 2 s later.
2. Drag an indoor lamp across a room with a table, stairs and a fridge. **Expected:** no blink on furniture or stairs, metal
   follows, no step at the release.
3. Drag a lamp standing on a table. **Expected:** the table's shadow follows it, with no flicker.
4. Ceiling lamp over furniture. **Expected:** the soft shadows of 2.18.4, and under a table a faint soft foot at each leg, never a hard streak.
5. Switch Lamp lighting to Classic and back, and Night Lights off and on. **Expected:** kept rooms freed, no stale light.
6. Change worlds. **Expected:** no rooms of the old world drawn.
7. Save, quit, load with the physical model. **Expected:** the same light.
8. Heavy lot. **Expected:** load time and memory within those of 2.18.4 plus the kept rooms' budget.
9. Lamp through a door, an arch and a diagonal window in Classic. **Expected:** the opening's shape on the floor.
10. Compare furniture at night against 2.18.4. **Expected:** brighter, no white burn.
11. Play past 5 a.m. in Live mode, then until dusk, with lamps lit indoors and outside. **Expected:** the light through the windows fades
    over a few seconds, by day lamps stay at their 3%, and nothing cuts in a frame.
12. Drag a table and a sofa in Build mode, in a room with a lamp on. **Expected:** the shadow follows the object, and the `[LiveTable]`
    lines log the move rate and thread.
13. A box-type sofa beside a low lamp, `chair_arm_caps` on and off. **Expected:** no thin streak past the seat, and the ceiling lamp's
    soft shadow unchanged.
14. Chairs under a ceiling lamp, `sphere_cut_fade` on and off. **Expected:** no crisp round arc at a shadow's end, and the same soft shadow.
15. Two hours of play with lot changes, Build mode, travel and a device reset. **Expected:** lamps still follow an edit in the frame; no
    `once a minute` line stays in the log with the light stuck.

## Confirmed in game

On developer builds of the code before the last review fixes, on one NVIDIA GPU: lamp drags held out of the solves (0 escaping
in the 2026-11-04 recording), the stain under a moved lamp gone, rigs at their final values after the release.

## Open checks

- No run of a public build; none of the last review's fixes (non-Steam builds, Classic, Night Lights off, world change) in game.
- Shader cache: regenerated from the release objects, checked on native D3D9 only.
- Rows beyond a kept atrium wall (`GhostKeptPS`, 3.1.1): not seen in game. Enter a lot with an atrium at night, take F8 within
  3 s of its lamps coming on: no dark band under the floor line. The shader text changed, so every `rt_lamps` shader (all 28,
  not only `GhostKeptPS`) misses the cache built into the ASI and compiles at start; under Wine / Proton one the builtin
  compiler refuses turns the GPU lamps off. Regenerating `resources/ApexRadiance_ShaderCache.bin` (release step 0) blocks the
  release. The lower story's dark windows of the same captures (2026-11-10) are not fixed: no capture recorded their rigs, and
  stage 5 wrote no directional map for the switch-ons at 10:01:49.6 (its reach test, `StartPhysBasis`, met no kept room's samples; such
  edits now log `reach no directional sample of the N kept rooms`), until room 19's kept solve wrote them at 10:01:54.7.
- None of the last jobs in game: dawn and dusk, object moves, sofa arms, long-session retries, new defaults on a real update.
- The relight cost of live tables and of `T3dDensePS` on big lots; whether the move hook fires at every drag step, which decides if the
  carry can go. The dawn fade rests on `sky+0x6B4` read from three snapshots.
- A deleted object whose memory is reused can show wrong geometry until its solve.
- Glass on room 0 (see Limitations): Scene replay records no room 0, so nothing is measured.
- Drag GPU cost in game: the benchmark's 31% is offline; a 2026-11-05 recording had 29 ms a round.
- A moved lamp's directional part from the solve's own code (`BasisPartNow`): no recording yet shows one tone through the keep.
- Furniture lamp gain: the factor 2 is an estimate; whether the Counters shader reads `CounterLightingConfig.y` is unknown.
- Table legs, stair set, baked opening masks and the shadow rewrite: offline only. No recording of a lamp dragged alone off its table.
- Leg shadows under high lamps: the heights and the strength are unverified, and at 0.095 under a lamp at 2.6 m they may read as too weak; `kPostHighM` is the value to raise.
- A lamp crossing into room 0 goes to `RelightLot`, after which edits are refused.
- Saving and loading, Replica against Physical side by side, memory and load time on heavy lots, AMD, Intel and DXVK. Quads rely on
  `VPOS` with pre-transformed vertices, tested on NVIDIA.
- `MigrateRoomLamps` has no test of its own.
- Whole room-0 tiles beside a diagonal wall (`AddFloorGridTwins`): a deck beside a diagonal wall should show a lamp's light with no steps. Offline: `rt_lamps_test`, and room 8 of the 05/11 23-40-21 scene gives 7 tiles whose copies land on the map's 4 x 4 holes. Not run in game; room 0's own texels were not recorded.

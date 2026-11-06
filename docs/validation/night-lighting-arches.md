# Light through arches and doorways: validation

The feature is described in [features/night-lighting/arches.md](../features/night-lighting/arches.md). It must:

- Show the brighter of two room ambients on an arch's line, fade the difference to none at the blend's reach (1.5 to 4 m), and never darken a
  side or a channel where the neighbour's ambient is lower.
- Match a draw to its room when the drawn `c4` differs slightly from the stored ambient, and not match another room.
- Give a floor-level sill no soft rim; keep window sills soft.
- Spread a lamp cone's edge through an arch over at most 0.3 m and cut the step between texels by at least one third,
  with the cone's middle at full light.
- Keep indoor light through openings independent of the camera.
- Draw no new step where a longer blend meets the room's next arch, and let blend and bounce together reach at most the neighbour's level.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/arch_ambient_test/` | `arch_ambient.h` with the real ambients of rooms 3 and 8: the band, the match tolerance, never darker, `Reach`, `HeldReach`, blend plus bounce | `run.ps1` | MSVC x86 |
| `tools/room_map_smooth_check/` | The arch variant of the floor and wall copies with 6 segments, created on native D3D9; a pixel within 1 LSB of the CPU formula, at a reach of 3.25 m too; channels where the neighbour is lower unchanged | `run.ps1 -Precomp <Shaders_Win32.precomp>` | Native D3D9, the game's shader package |
| `tools/roofed_openings_test/` | Floor-level sill (0.559 to 1.000); `ArchConeChecks`: cone step 0.806 to 0.577, edge 0.3 m or less; hard wall tops under a ceiling | `run.ps1` | MSVC x86 |
| `tools/rt_lamps_test/`, `tools/outdoor_delta_test/` | Joined tables (`BlockerTable::Join`), panes, `ToneSoft` and the bounce on the GPU and on the CPU | `run.ps1` | GPU |

## Latest results

Release validation for `v2.15.6`: Map slot replacement, relocation and unreadable memory; constant state preservation and existing arch checks. The reviewed implementation is `f66e777`;
final release build and gameplay confirmation are tracked with the release tag and PR. Offline results do not establish
an FPS gain, complete climate coverage or resolution of texture allocation stalls.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-06 | tree | `room_map_smooth_check` with the game's `Shaders_Win32.precomp`, neighbour's cube | 2,022 checks, 0 failures. 120 of 120 arch copies created natively, 52 with the neighbour's cube, 68 with no cube, at most 21 temps. Pixels: 1 LSB against the CPU law, 0 LSB off the neighbour on the line and off the own cube past the reach | Native D3D9 |
| 2026-11-06 | tree | `arch_ambient_test`, `TakeCube` included | 4,163 checks, 0 failures | Offline |
| 2026-11-05 | `1f76e83` with the 3.0.0 working tree | `rt_lamps_test`, `outdoor_delta_test`, `arch_ambient_test` | 137, 217 and 4,153 checks, 0 failures; blend plus bounce at most 1.000 of the difference; the dense parity check found 0 sums different for a room's own lamps | MSVC x86; NVIDIA |
| 2026-11-05 | `1f76e83` with the 3.0.0 working tree | `room_map_smooth_check` with the game's `Shaders_Win32.precomp` | 1,686 checks, 0 failures, the GPU within 1 LSB of the CPU law at a reach of 3.25 m. Run before `HeldReach` and `ArchTwin`, not run again: `shader_patches.cpp` did not change after it | Native D3D9 |
| 2026-11-05 | `1f76e83` with the 3.0.0 working tree | Reach, offline, a 2.66 m arch (table below) | Steepest fall per 25 cm 26.5% to 11.3% | Offline |
| 2026-11-05 | `1f76e83` with the 3.0.0 working tree | Simulated floors, production headers, scene 19-37-16 (table below) | Seam ratio median 1.90 to 1.00, columns over 1 level 18 to 0 of 19 | Offline |
| — | `v2.15.6` (implementation `f66e777`) | `arch_ambient_test; render_cost_test; lighting_prepare_test` | 4121 arch checks; 32768 constant blocks; 16 map-slot cases passed | CPU fixtures |
| 2026-10-22 | `823e339` | None: `arch_ambient_test` covers `arch_ambient.h`, not the story memo of `ArchAmbientConsts` | Not run for the memo | not run in game yet |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/arch_ambient_test/` | 25 checks passed | None (pure) |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/room_map_smooth_check/` | 1084 checks passed | Native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/roofed_openings_test/` | 196 checks passed | None (pure) |

Simulated floors, before and after. The doorway's table position is an estimate, so it shows the mechanism, not a pixel match.

| | Kitchen, green lamp: before | after | Doorway, table moved: before | after |
|---|---|---|---|---|
| Seam ratio across adjacent texels, median / max | 1.90 / 2.88 | 1.00 / 1.17 | 5.79 / 6.26 | 1.11 / 1.78 |
| Columns stepping over 1 level past the gradient | 18 of 19 | 0 of 19 | 12 of 19 | 1 of 19 |
| Kitchen floor mean | 0.0139 | 0.0167 | 0.0799 | 0.0642 |
| Kitchen dark floor | 0.0098 | 0.0104 | 0.0103 | 0.0164 |

On the kitchen case, the wedge median goes from 0.0238 to 0.0263, pairs where more light showed darker from 39 of 803 to 0, all 769 lit
texels stay lit, 114 of 2,560 texels are over 1 level darker (all behind glass) and the hall mean moves from 0.0840 to 0.0838. The bounce's
largest step between texels is 0.15 level, and it adds at most 0.9. The doorway's remaining column is a shadow edge crossing the line.
`scene_replay` did not move for these jobs, but scenes recorded before scene format 2 hold no glazed flags, so it does not exercise the glass.

The blend's reach on the 2.66 m arch (18.06, 10.06)-(19.94, 11.94), at 3.33 m, a light-map alpha of 0.816. The lift is the mean over a 5 x 5 m
dark room, as a share of its own ambient. The brighter room does not change, and the dark room keeps its own ambient past the reach.

| Room ambients | Steepest fall per 25 cm, 1.5 m to the reach | Per 6.25 cm texel | Mean lift |
|---|---|---|---|
| 0.0116 and 0.0288 | 26.5% to 11.3% | 6.13% to 2.72% | 18% to 47% |
| 0.0119 and 0.0322 | 29.6% to 12.6% | 6.79% to 3.01% | 21% to 54% |

The lift grows with the length. `kReachPerWidth` at 1.0 gives 14.3% per 25 cm and a lift of 36%. Room 3 of the recorded lot has its arches over 5 m
apart, so `HeldReach` leaves its reach as it is. A room at 0.0116 with a 0.0288 neighbour and a 0.0125 room behind an arch 2 m away showed a
1.41 times step on that line before `HeldReach`.

Steady indoor light has no offline test: it is a condition in the game's path. Its evidence is a recording in which the
rooms near openings are no longer solved again every one to two seconds.

## In-game test plan

1. Look at the floor through an arch between two differently lit rooms for 30 s while rotating the camera.
   **Expected:** no step on the arch's line at any angle.
2. Change stories several times, return to the arch's story and wait 10 s. **Expected:** the step does not come back.
3. A lamp lighting the next room through an arch and through a window. **Expected:** a soft edge, with the shape of the
   arch and the window still recognisable.
4. A recording while the camera moves with no lamp edit. **Expected:** no periodic room invalidations.
5. Switch a lamp beside an opening off. **Expected:** the rooms on both sides update within about half a second.
6. The kitchen doorway of the 2026-11-05 F7 shots with the lamp still, with a Recording. **Expected:** no line of 2 to 4 pixels on the
   doorway's floor; the Recording's wedge lines tell its cause if it stays.
7. A table beside the doorway, lit from the next room. **Expected:** its shadow crosses the line of the arch.
8. *Light bounces between rooms* off and on with a lit room beside a dark one. **Expected:** a soft fill near the arch, no band.
9. A red lamp seen through two windows. **Expected:** dimmer than 2.18.4, still present, no black cut past the second pane.
10. A wide arch between a bright room and a dark one. **Expected:** the dark room's floor rises slowly over up to 3 to 4 m, no band, no step at
    the room's next arch, and no floor tone that looks detached from the wall beside it.

## Confirmed in game

- Nothing yet. In a development session the drawn `c4` values matched the logged ambients of rooms 3 and 4, and the
  transition was applied on thousands of floor draws; the visual result is not confirmed.

## Open checks

- The appearance in game of every part.
- The neighbour's cube: not run in game and not run on DXVK. The line needs a new F7 pair, and `RoomSpecCube` has no offline test: it reads engine memory.
- The line of 2 to 4 pixels at a doorway: not explained, and only a capture can say if it left.
- A room holding the very ambient of a room with an arch takes the blend through a wall within 1.5 m: not checked on a real lot.
- `ArchTwin` has no offline test: it reads engine memory. Any room sharing an ambient on the story holds the blend at 1.5 m, and lampless rooms
  often share one, so how many dark rooms keep the steeper fall is not known.
- The reach's four numbers, the larger lift of the dark room, and a floor lifted up to 4 m in beside a wall that keeps its step: none seen in game.
- Whether the receiving room should also follow later changes of the source room's normalisation.
- Whether floors with more than 6 arch and opening segments occur and lose a transition.
- Story memo of 16 room maps (`823e339`): a house with arches on two stories, both in view (open stairwell or cutaway).
  **Expected:** each story's floors keep their own arch transition, the same as before; the F8 snapshot's draws of no
  known room do not grow.

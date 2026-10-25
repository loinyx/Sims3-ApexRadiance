# Light through arches and doorways: validation

The feature is described in [features/night-lighting/arches.md](../features/night-lighting/arches.md). It must:

- Show the brighter of two room ambients on an arch's line, fade the difference to none at 1.5 m, and never darken a
  side or a channel where the neighbour's ambient is lower.
- Match a draw to its room when the drawn `c4` differs slightly from the stored ambient, and not match another room.
- Give a floor-level sill no soft rim; keep window sills soft.
- Spread a lamp cone's edge through an arch over at most 0.3 m and cut the step between texels by at least one third,
  with the cone's middle at full light.
- Keep indoor light through openings independent of the camera.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/arch_ambient_test/` | `arch_ambient.h` with the real ambients of rooms 3 and 8: the band, the match tolerance, never darker | `run.ps1` | MSVC x86 |
| `tools/room_map_smooth_check/` | The arch variant of the floor and wall copies with 6 segments, created on native D3D9; a pixel within 1 LSB of the CPU formula; channels where the neighbour is lower unchanged | `run.ps1 -Precomp <Shaders_Win32.precomp>` | Native D3D9, the game's shader package |
| `tools/roofed_openings_test/` | Floor-level sill (0.559 to 1.000); `ArchConeChecks`: cone step 0.806 to 0.577, edge 0.3 m or less; hard wall tops under a ceiling | `run.ps1` | MSVC x86 |

## Latest results

Release validation for `v2.15.6`: Map slot replacement, relocation and unreadable memory; constant state preservation and existing arch checks. The reviewed implementation is `f66e777`;
final release build and gameplay confirmation are tracked with the release tag and PR. Offline results do not establish
an FPS gain, complete climate coverage or resolution of texture allocation stalls.

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| — | `v2.15.6` (implementation `f66e777`) | `arch_ambient_test; render_cost_test; lighting_prepare_test` | 4121 arch checks; 32768 constant blocks; 16 map-slot cases passed | CPU fixtures |
| 2026-10-22 | `823e339` | None: `arch_ambient_test` covers `arch_ambient.h`, not the story memo of `ArchAmbientConsts` | Not run for the memo | not run in game yet |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/arch_ambient_test/` | 25 checks passed | None (pure) |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/room_map_smooth_check/` | 1084 checks passed | Native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/roofed_openings_test/` | 196 checks passed | None (pure) |

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

## Confirmed in game

- Nothing yet. In a development session the drawn `c4` values matched the logged ambients of rooms 3 and 4, and the
  transition was applied on thousands of floor draws; the visual result is not confirmed.

## Open checks

- The appearance in game of every part.
- Whether the receiving room should also follow later changes of the source room's normalisation.
- Whether floors with more than 6 arch and opening segments occur and lose a transition.
- Story memo of 16 room maps (`823e339`): a house with arches on two stories, both in view (open stairwell or cutaway).
  **Expected:** each story's floors keep their own arch transition, the same as before; the F8 snapshot's draws of no
  known room do not grow.

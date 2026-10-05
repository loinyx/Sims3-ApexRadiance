# Walls: validation

The feature is described in [features/night-lighting/walls.md](../features/night-lighting/walls.md). It must:

- Change only the `.x` of the recognised lamp-scale constant, for one draw, and restore the full constant afterwards.
- Keep the native draw for unknown shaders, failed or invalid constant reads, an unchanged result and the switch off.
- Return exactly `native x Brightness` at full night and add only a subdued lamp term by day.
- Leave sun, sky, wall-map alpha, textures and occlusion unchanged.
- Light the wall below an upper-story sconce continuously across the floor line (see
  [level-light-share validation](night-lighting-level-light-share.md)).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) (`run.ps1`, `run_resource_checks.ps1`) | Extracted wall draw path against D3D9 fixtures: exact gain and no-op writes, toggles, failed original-constant reads, invalid factors, restoration of every touched state; the captured ExteriorWall pixel shader on native D3D9 with controlled materials and maps (old zero-factor failure reproduced, lamp RGB read back by day and twilight, full-night pixels identical, empty lamp maps unchanged) | See the harness README | MSVC x86, native D3D9, captured shaders |

The harness reads only the repository and the given capture folder and prints to the console. It is not a visual
validation of a player's lot and does not establish instant wall-map baking.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | PR #2 (`92582b6`) | Wall draw fixtures and captured-shader daytime checks | Passed | Native D3D9 |

## In-game test plan

1. A house with a sconce on the upper outside wall, at night. **Expected:** no straight cut at the floor line; the side
   wall around a corner is not brighter below than above.
2. Developer page status *Walls*. **Expected:** `outside walls: strength 2.00 | draws: N | variants seen: M`, with N
   growing while walls are on screen at night.
3. *Lamps light walls* off. **Expected:** walls return to the game's brightness, by day and night.
4. F7 on a lit wall. **Expected:** a table PS (1372 bytes for `ExteriorWall_PS_1119`) and the draw's c3.x (or c2.x)
   multiplied.
5. Daytime near a lamp at 100% and 200%. **Expected:** a subdued lamp pool on the wall, not saturated; at dusk it blends
   into the night value without a jump.
6. Status *Stories* and F8 after 10 s still: the story section lists room 0 of each level with the same outdoor lights.

## Confirmed in game

- Combined build, 2026-09-25: lamps of every story with the cross-story wall test confirmed after the second round.
- 2026-10-04, daytime F7 10:28:03: `PS_265EBE18.bin` (1372 bytes, `04956FE9`, K = 3) with `c3 = (0, 0.188235313, 0, 0)`
  confirms the native daytime factor discards the baked lamp RGB (the map holds non-zero RGB elsewhere; this is not
  proof of illumination at that pixel). The 11:13 follow-up confirmed full-day c3.x = 1 with the then-installed build.

## Open checks

- Visual approval of the 8% daytime term (session 11-32-34 stayed at night and could not show a daytime failure).
- Whether `PS_2A74E378` (wall family, baked `texld s2 x c3.x`, refused by the object patch) is in `wall_lamp_table.h` (its
  content hash was not computed).

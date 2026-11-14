# ReShade/GShade looks: validation

The feature is described in [features/gshade-looks.md](../features/gshade-looks.md). It must:

- Run the shaders of the chosen preset in the preset's order, with the preset's values, and leave the picture unchanged
  when the card is off.
- Never run blur, anti-aliasing, distortion or depth-based shaders over the HUD.
- Run once per game frame, also when the look is split around the scene boundary.
- Skip a shader that needs depth when no scene depth is available, and name it in the status.
- Skip DLSS5_Feed and DLSS5_Feed_Debug, whatever the preset says.
- Keep the edited values, switches and run order across restarts, and go back to the .ini on *Back to the .ini*.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/gen_gshade_shaders.py` | Every built-in port compiles for ps_3_0 with fxc | `python -I tools/gen_gshade_shaders.py` | fxc, a GShade or ReShade shader folder (`GSHADE_SHADERS`) |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-14 | PR branch | `gen_gshade_shaders.py` | 20 shaders, every entry point compiles | None |

## In-game test plan

1. Image > Color > ReShade/GShade, switch on with the default preset. **Expected:** the look shows on the scene; the
   HUD text stays sharp.
2. Pick another preset in the picker. **Expected:** its shaders are listed in the .ini's order and the look changes.
3. Move a slider, restart the game. **Expected:** the edited value is kept; *Back to the .ini* restores the .ini value.
4. A preset with a depth shader, in a world and in the map view. **Expected:** no shade over the HUD; in the map view the
   shader is listed as skipped (no depth).
5. Ctrl+Shift+L. **Expected:** the looks turn off and on, as the card switch does.

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan.

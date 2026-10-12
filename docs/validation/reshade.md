# ReShade effects: validation

ReShade `.fx` effects must compile and run without ReShade installed, render on the finished scene before the game's
interface (never over menus and text), read Apex Radiance's depth as `DEPTH`, and keep their switches and values in a
ReShade preset. Files added in the menu must load. The feature is described in [features/reshade.md](../features/reshade.md).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/reshade_fx_test`](../../tools/reshade_fx_test/fx_check.cpp) | Preprocess, parse and shader model 3 code generation of every `.fx` below the given folders; on Windows also `D3DCompile` (`vs_3_0` / `ps_3_0`). The repository's test effects (`tools/reshade_fx_test/effects`, run with `--expect-ok`) must all compile | `tools/development/check.ps1` (Smoke) | none |
| none | The ZIP reader was checked offline against deflate and stored packs, with a `..` entry rejected | | none |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-12 | before the runtime commit | `fx_check` (Linux, parse only) | 108 of 108 effects from SweetFX, prod80, qUINT, AstrayFX, Depth3D, reshade-shaders and fubax parsed and generated shader model 3 code | none |

## In-game test plan

1. No ReShade installed, feature on, add a `.zip` pack (for example SweetFX) and turn on a colour effect. **Expected:**
   files under `Apex Radiance\ReShade`, the effect listed with its settings, the world graded and the game's menus,
   plumbobs and text not; no error in the log.
2. A depth effect (MXAO, ADOF, a fog). **Expected:** depth follows the world, not the interface; the Developer page shows
   "Scene depth requested: yes" only while it is on.
3. Loading screen and main menu. **Expected:** no effect over them.
4. Game Edge Smoothing on. **Expected:** effects still under the interface.
5. Change a slider, switch the preset, restart the game. **Expected:** the preset and its values persist; the `.ini`
   opens in ReShade with the same effects on.
6. An effect that fails (compute shader, too many registers). **Expected:** the error in its panel and Remove works.
7. Resize the window or change resolution. **Expected:** effects reload for the new size; no lost device.
8. DXVK. **Expected:** same as step 1.

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan.
- `D3DCompile` acceptance of the public packs' effects (Windows run of `fx_check` over the packs).
- GPU cost of common effects at 1080p.

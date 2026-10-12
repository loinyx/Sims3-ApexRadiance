# ReShade effects: validation

ReShade's effects must render on the finished scene before the game's interface, never over menus and text, with Apex
Radiance's depth when requested, and files added in the menu must load. The feature is described in
[features/reshade.md](../features/reshade.md).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| none yet | The ZIP reader was checked offline against deflate and stored packs, with a `..` entry rejected | | none |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | | |

## In-game test plan

1. ReShade 6.3+ for Direct3D 9 installed, feature on, an effect such as a sepia or LumaSharpen on. **Expected:** the
   world is graded, the game's menus, plumbobs and text are not; `[ReShade] Connected` in the log.
2. A depth effect (MXAO, ADOF) with **Accurate distance for effects** on, then off. **Expected:** depth follows the world
   with it on; ReShade's own detection after the reload with it off.
3. Loading screen and main menu. **Expected:** no effect over the loading screen or menus.
4. Game Edge Smoothing on. **Expected:** the multisampled note; ReShade at its default point.
5. Add a `.zip` shader pack and a single `.fx`. **Expected:** files under `Apex Radiance\ReShade`, the search paths
   appended once in `ReShade.ini`, the effects listed after the reload.
6. Switch the ReShade preset in the tab, change a slider, reopen the game. **Expected:** the preset and value persist.
7. DXVK installed with ReShade on Vulkan. **Expected:** the DXVK note; ReShade unchanged.
8. ReShade older than 6.3. **Expected:** the update note; ReShade unchanged.

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan.
- Whether the back buffer surface Apex passes is ReShade's own back buffer (default permutation) or creates a second
  effect permutation.

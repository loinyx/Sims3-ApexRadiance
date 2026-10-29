<p align="center">
  <img src="docs/images/logo-256.png" width="120" alt="Apex Radiance logo">
</p>

<h1 align="center">Apex Radiance</h1>

<p align="center"><b>A lighting overhaul and graphics mod for The Sims 3</b></p>

<p align="center">
  <a href="https://github.com/loinyx/Sims3-ApexRadiance/releases/latest"><img alt="Latest version" src="https://img.shields.io/github/v/release/loinyx/Sims3-ApexRadiance?label=version&color=7F77DD"></a>
  <a href="https://github.com/loinyx/Sims3-ApexRadiance/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/loinyx/Sims3-ApexRadiance/total?color=534AB7"></a>
  <img alt="The Sims 3: Steam 1.67.2 or EA app 1.69" src="https://img.shields.io/badge/The%20Sims%203-Steam%201.67.2%20%7C%20EA%20app%201.69-534AB7">
  <a href="LICENSE"><img alt="MIT license" src="https://img.shields.io/github/license/loinyx/Sims3-ApexRadiance?color=534AB7"></a>
</p>

<p align="center">
  <a href="https://github.com/loinyx/Sims3-ApexRadiance/releases/latest"><b>Download</b></a> ·
  <a href="https://www.nexusmods.com/thesims3/mods/247">Nexus Mods</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#compatibility">Compatibility</a> ·
  <a href="#reporting-a-problem">Report a problem</a>
</p>

<p align="center">
  <img src="https://github.com/user-attachments/assets/82856ded-a8b7-4c23-a3e9-5c3ed1631b40" width="100%" alt="A community lot at night, before and after Apex Radiance">
</p>

The Sims 3 lights its world in separate pieces: the ground, each lot, every floor of a house, the objects, the roofs and the water all get their lamp light from a different system. That is why street light stops in a straight line at the edge of a lot, why a balcony lamp leaves the porch below it dark, and why roofs and ponds turn black at night.

Apex Radiance rebuilds that lighting so it works as one. A lamp lights everything around it, from the furniture in its room to the floors above and below, the lawn, the street and the lot next door. On top of that come graphics effects the game never had: ambient occlusion, water reflections, a banding fix, colour filters, edge smoothing and depth of field.

## Lighting

<table>
<tr>
<td width="50%"><img src="https://github.com/user-attachments/assets/c2fb9c34-8545-49f4-a3ac-a65e9086de86" width="100%" alt="A neighbourhood at night, before and after"></td>
<td width="50%"><img src="https://github.com/user-attachments/assets/5902b18d-f29a-44b3-9cec-6817192344ac" width="100%" alt="A street lamp beside a lot, before and after"></td>
</tr>
<tr>
<td align="center"><sub>Street and lot lamps light the whole neighbourhood</sub></td>
<td align="center"><sub>Street light reaches the lawn and the houses past the lot edge</sub></td>
</tr>
</table>

### Outside

- Street lamps light the lots beside them, and lot lamps light the grass, roads and sidewalks past the lot edge.
- The ground gets soft, round pools of light instead of the game's 1-metre blocks.
- Walls, foundations, roofs, trees and garden objects take the light of the lamps near them.
- Walls cast lamp shadows on the ground; chain-link fences, lattices and railings let the light through their gaps.
- Rain, snow and wet roads keep the right lamp light, and the ground relights when the lamps come on at dusk.

### Inside and between floors

<table>
<tr>
<td width="55%" valign="top">

- Indoor light reaches the floors above and below through stairwells and open floors, and stops at solid floors.
- Lamps on balconies and upper floors light the ground below them.
- Light passes through doors, windows and arches and fades across the next room.
- Furniture, stairs, railings and columns light up evenly, and walls meet between floors without seams.

</td>
<td width="45%" valign="top"><img src="https://github.com/user-attachments/assets/3b748f76-115e-414e-831c-c55fc2e14134" width="100%" alt="Rooms of a building at night, before and after"><br><sub>Light fills the rooms and floors of a building</sub></td>
</tr>
</table>

### While you build

Move, add or delete a lamp in Build mode and the light follows right away: walls, floors, furniture and the ground update while you drag. This runs on your graphics card (**Lamp updates: Instant**, the default); on a weaker PC, **Classic** relights once you put the lamp down.

### Make it your own

Pick a lighting balance (**Subtle**, **Soft**, **Natural** or your own), then fine-tune the ground, walls, roofs, objects, light between floors and rooms at night. The corrections to the game's own lighting bugs sit on a separate **Game fixes** tab, on by default.

[How the lighting works, in detail →](docs/features/night-lighting/README.md)

## Water reflections

<table>
<tr>
<td width="45%" valign="top"><img src="https://github.com/user-attachments/assets/3d287975-5231-475e-9cb8-37342915c7db" width="100%" alt="A lake at night, before and after"><br><sub>Trees on the shore reflected in the lake</sub></td>
<td width="55%" valign="top">

In the base game, ponds and lakes reflect only a fixed picture of the sky, so they never show what stands around them.

With Apex Radiance they mirror the trees, houses and lamps along their shore, by day and by night, including the water of Twinbrook, Bridgeport and Moonlight Falls. At night, lamps near the water also add glints and a soft glow on its surface.

Water Reflections needs Depth Blur or Ambient Occlusion turned on.

</td>
</tr>
</table>

## Ambient Occlusion

The Sims 3 has no ambient occlusion, so furniture looks like it floats and rooms look flat. Apex Radiance adds soft shadows where surfaces meet: under furniture, in room corners, where walls touch the floor and around houses and trees.

- The picture stays steady when the camera is still, and lamp-lit surfaces keep their light and colour.
- Five quality levels, and it also works in map view.
- **Sim Occlusion** recognises Sims, custom content included, and gives their body and hair a lighter shade of their own, so faces never look dirty.

## Banding Fix

The game draws its world with only 256 shades per colour, so the soft pool of light a lamp throws on a wall breaks into rings, most of all in dark scenes and on OLED screens. Banding Fix draws the 3D world with far more shades and finishes it with a fine, invisible grain, so light fades smoothly across walls, floors, shadows and skies.

## Color, filters and LUTs

**Filters never touch the game's interface.** They apply to the 3D world only, so pie menus, the HUD, tooltips and plumbobs keep their own colours and stay sharp.

The Color page goes from small corrections (brightness, contrast, saturation, temperature, tones, a six-colour mixer, clarity, vignette) to a completely new look. Turn on as many of its 24 filters as you like and they add up:

| Family | Filters |
|---|---|
| Film looks | Technicolor 1 and 2, DPX Cineon, Vintage, Cross-process, Filmic pass, Black and white, Tint |
| Color and mood | Colorfulness, Night Mode, Levels |
| Light and detail | Auto exposure, Adaptive sharpening, Glow, Halation, Dreamy, Fake HDR |
| Camera | Emphasize, Tilt-shift, Prism, Film grain |
| Retro and style | 3DFX, CRT |
| Accessibility | Color-blind mode (red, green or blue colour blindness) |

**LUTs** give the game a ready-made colour grade, the same kind photographers and film editors use. Put a 3D CUBE file (the format Photoshop, DaVinci Resolve and most LUT packs export) or a PNG strip (the layout ReShade LUTs use) in the Apex `LUTs` folder, pick it and set how strongly it applies.

Every filter has its own switch and keyboard shortcut (right-click it), and with the menu open, holding **B** shows the picture without your colour changes.

## Edge Smoothing and Depth Blur

**Neither touches the game's interface.** Both run before the game draws it, so pie menus, tooltips, plumbobs and the HUD stay sharp.

- **Edge Smoothing** removes the jagged edges of roofs, fences, walls and furniture with FXAA (lighter) or SMAA (cleaner long edges), plus optional texture sharpening. Turn off the game's own Edge Smoothing under **Options > Graphics** when you use it.
- **Depth Blur** softens the distant background like a camera lens. Lamps in the background stay bright, and the blur fades out in map view.

## Performance

Fifteen optional switches make the game's heaviest work faster or spread it over several frames, without changing how it looks:

- faster file and object lookups (the game searches around 290 files for each thing it loads);
- room lights that settle sooner, and lot and wall updates spread out while the camera moves;
- faster texture and cache compression, and faster sorting in Create a Sim;
- lighter memory handling, window redraws and scripts.

**Room to save** keeps a block of memory free and hands it back right before each save, which makes Error 12 less likely (no mod can rule it out). **Lot Streaming**, experimental and off by default, keeps lots detailed farther away.

[All performance options →](docs/features/performance/README.md)

## Presets and shortcuts

Start from a built-in preset (**Performance**, **Balanced** or **Quality**), save your own looks, and share them as a TOML file or a ZIP with their LUT. A preset shows **Applied** while your settings still match it.

| Key | What it does |
|---|---|
| **Ctrl+Shift+F11** | Open the menu, once a world has loaded |
| **Ctrl+Shift+F10** | Turn Apex's effects off and on, to compare with the game |
| **Ctrl+Shift+F9** | Refresh the lighting |
| **F8** | Save a screenshot with Apex's effects |
| **F10** | Hide the game interface |

The menu is in 21 languages and follows your Windows language until you pick one in Settings. Click its version number to see **What's new**.

## Installation

1. Install an ASI loader in `The Sims 3\Game\Bin`, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
2. Close the game, then copy `ApexRadiance.asi` from the [latest release](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest) into the same folder, replacing the old file when you update.
3. Load a world and press **Ctrl+Shift+F11**.

Apex Radiance supports Steam **1.67.2** (`TS3W.exe`) and EA app **1.69** (`TS3.exe`). Updating keeps your settings, which live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\`.

<details>
<summary><b>Steam Deck and Linux</b></summary>

Apex runs through Proton with the same file. Add the loader's DLL override to the game's launch options in Steam, as described in [Steam Deck and Proton](docs/features/steam-deck.md#installing-on-steam-deck-or-linux).

</details>

<details>
<summary><b>Something looks broken after a change</b></summary>

**Settings → Compatibility → Recovery → Restart Apex** rebuilds the image effects without touching your settings. It does not uninstall the mod or reset the lighting.

</details>

<details>
<summary><b>Coming from an older combined build</b></summary>

Remove the old combined ASI or `S3SSApex.asi` from `Game\Bin`, and keep the official standalone `Sims3SettingsSetter.asi` if you use it. Apex imports your old settings on its first start.

</details>

## Compatibility

Recommended together with Apex Radiance:

- **[Sims3SettingsSetter](https://github.com/sims3fiend/Sims3SettingsSetter/releases)**: Apex detects its Split-Level Lighting Fix and doesn't apply it twice. Don't use its experimental features together with Apex.
- **[Sims 3 Modernization](https://github.com/rodneypantonial/sims3-modernization/releases)**: Room to save recognises its save hook and still prepares the save before it.
- **[Sims 3 Performance Patch](https://www.nexusmods.com/thesims3/mods/235)**: with Sims 3 Modernization, turn off its `ShortWaitSpin` and `ThreadStackDiet`; Apex's **Settings > Compatibility** offers to do it for you.
- **[DXVK](https://github.com/doitsujin/dxvk/releases)**: supported, though results can vary between graphics backends.

## Reporting a problem

Open **Report a problem** in the Apex menu to save a report, record a short clip or capture the lighting on a surface. Zip its folder from `Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\` and attach it to a [GitHub issue](https://github.com/loinyx/Sims3-ApexRadiance/issues) or the Bugs tab on [Nexus Mods](https://www.nexusmods.com/thesims3/mods/247), with what happened, how to make it happen again, your game version and whether you use DXVK.

## Credits

- **Apex Radiance** by [@loinyx](https://github.com/loinyx).
- Sims3SettingsSetter by sims3fiend.
- Every-Story Ground Light (lamps on upper floors lighting the ground) uses a technique from [Arro](https://arro-now.tumblr.com/)'s Split-Level Lighting Fix.
- Lot Streaming and the daytime bloom fixes: research and code by [idavidveiga](https://github.com/idavidveiga).
- Edge Smoothing's FXAA mode follows FXAA 3.11 by Timothy Lottes (NVIDIA).
- Scene update safety for effects and animal fur by [@rodneypantonial](https://github.com/rodneypantonial) ([PR #38](https://github.com/loinyx/Sims3-ApexRadiance/pull/38)).
- Thanks to @boringbones for testing every build.

<details>
<summary><b>Third-party code, documentation and building from source</b></summary>

Third-party code: [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [Microsoft Detours](https://github.com/microsoft/Detours) (MIT), [toml++](https://github.com/marzer/tomlplusplus) (MIT), [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al. (see `third_party/smaa/LICENSE.txt`), the texture sharpening after [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) (MIT), [Lucide](https://lucide.dev) icons (ISC, see `third_party/lucide/LICENSE`).

The [documentation](docs/README.md) explains every feature, its settings and how it was tested; the [release notes](docs/releases/README.md) list the changes in each version.

To build, use Visual Studio 2022 Build Tools (v143), C++20 and the x86 Release target, with a static CRT and vcpkg's `x86-windows-static` dependencies (Dear ImGui, Microsoft Detours, toml++). The [build workflow](docs/workflow.md#1-build) has the details.

</details>

The Sims is a trademark of Electronic Arts Inc. This is a fan-made mod, not affiliated with or endorsed by Electronic Arts. Licensed under [MIT](LICENSE).

<p align="center">
  <img src="docs/images/logo-256.png" width="140" alt="Apex Radiance logo">
</p>

<h1 align="center">Apex Radiance for The Sims 3</h1>

<p align="center"><b>A brighter world. A night worth looking at.</b></p>

<p align="center">
  <a href="https://github.com/loinyx/Sims3-ApexRadiance/releases/latest"><img alt="Latest version" src="https://img.shields.io/github/v/release/loinyx/Sims3-ApexRadiance?label=version&color=7F77DD"></a>
  <a href="https://github.com/loinyx/Sims3-ApexRadiance/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/loinyx/Sims3-ApexRadiance/total?color=534AB7"></a>
  <img alt="The Sims 3: Steam 1.67.2 or EA app 1.69" src="https://img.shields.io/badge/The%20Sims%203-Steam%201.67.2%20%7C%20EA%20app%201.69-534AB7">
  <img alt="Menu in 21 languages" src="https://img.shields.io/badge/menu-21%20languages-534AB7">
  <a href="LICENSE"><img alt="MIT license" src="https://img.shields.io/github/license/loinyx/Sims3-ApexRadiance?color=534AB7"></a>
</p>

<p align="center">
  <a href="https://github.com/loinyx/Sims3-ApexRadiance/releases/latest"><img alt="Download on GitHub" src="https://img.shields.io/badge/Download-GitHub-7F77DD?style=for-the-badge&logo=github&logoColor=white"></a>
  <a href="https://www.nexusmods.com/thesims3/mods/247"><img alt="Download on Nexus Mods" src="https://img.shields.io/badge/Download-Nexus%20Mods-7F77DD?style=for-the-badge"></a>
</p>

<p align="center">
  <a href="#whats-included">Features</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#compatibility">Compatibility</a> ·
  <a href="#reporting-a-problem">Report a problem</a> ·
  <a href="https://github.com/loinyx/Sims3-ApexRadiance/releases">Release notes</a>
</p>

The Sims 3 builds its lighting out of separate pieces that don't talk to each other. The ground, each lot, every floor of a house, the objects, the roofs and the water all get their lamp light from a different system. That is why street light stops in a straight line at the edge of a lot, why a balcony lamp leaves the porch below it dark, and why roofs and ponds turn black at night.

Apex Radiance rebuilds that lighting so it works as one. A lamp lights everything around it: the floor and walls of its room, the furniture, the floors above and below, the lawn, the street and the lot next door. The mod also adds graphics effects the game never had, such as ambient occlusion, water reflections and colour filters, along with optional fixes for stutters and memory.

<div align="center">

<table>
<tr>
<td>
<img src="https://github.com/user-attachments/assets/c2fb9c34-8545-49f4-a3ac-a65e9086de86" width="100%">
</td>
<td>
<img src="https://github.com/user-attachments/assets/3d287975-5231-475e-9cb8-37342915c7db" width="100%">
</td>
</tr>

<tr>
<td>
<img src="https://github.com/user-attachments/assets/3b748f76-115e-414e-831c-c55fc2e14134" width="100%">
</td>
<td>
<img src="https://github.com/user-attachments/assets/5902b18d-f29a-44b3-9cec-6817192344ac" width="100%">
</td>
</tr>

<tr>
<td colspan="2">
<img src="https://github.com/user-attachments/assets/82856ded-a8b7-4c23-a3e9-5c3ed1631b40" width="100%">
</td>
</tr>

</table>

</div>

## What's included

<table>
<tr>
<td width="50%" valign="top">
<b><a href="#lighting">Lighting</a></b><br>
Lamp light that reaches the ground, the walls, every floor and every object, without the game's cuts and blocks.
</td>
<td width="50%" valign="top">
<b><a href="#water-reflections">Water reflections</a></b><br>
Ponds and lakes mirror the houses, trees and lamps on their shore, by day and by night.
</td>
</tr>
<tr>
<td width="50%" valign="top">
<b><a href="#ambient-occlusion">Ambient Occlusion</a></b><br>
Soft contact shadows under furniture and in corners, with a separate, lighter shade for Sims.
</td>
<td width="50%" valign="top">
<b><a href="#banding-fix">Banding Fix</a></b><br>
Smooth light on walls and in the sky instead of rings.
</td>
</tr>
<tr>
<td width="50%" valign="top">
<b><a href="#color-filters-and-luts">Color, filters and LUTs</a></b><br>
Small corrections or a completely new look, with 24 stackable filters and your own LUTs, while the game's interface keeps its colours.
</td>
<td width="50%" valign="top">
<b><a href="#edge-smoothing-and-depth-blur">Edge Smoothing and Depth Blur</a></b><br>
Clean edges and a camera-like blur in the distance, with the interface left sharp.
</td>
</tr>
<tr>
<td width="50%" valign="top">
<b><a href="#performance">Performance</a></b><br>
Optional fixes for stutters and for saves that fail with Error 12.
</td>
<td width="50%" valign="top">
<b><a href="#presets-and-the-menu">Presets and the menu</a></b><br>
Built-in and shareable presets, keyboard shortcuts and a menu in 21 languages.
</td>
</tr>
</table>

## Lighting

Every surface of the game gets the light of the lamps around it, and the light no longer stops where one of the game's systems ends and the next begins.

| | The Sims 3 | With Apex Radiance |
|---|---|---|
| **Lot borders** | Street light stops in a straight line at the edge of a lot, and lot lamps never reach the street | Street lamps light the lots beside them, and lot lamps light the grass, roads and sidewalks past the lot edge |
| **Ground** | Lamp light is drawn in coarse 1-metre blocks with coloured specks | Soft, round pools of light on grass, roads, sidewalks and patios |
| **Floors** | Each floor sees only its own lamps, so light cuts off at the floor line | Light reaches the floors above and below through stairwells and open floors, and balcony lamps light the ground below |
| **Rooms** | Light stays in the room of its lamp | Light passes through doors, windows and arches and fades across the next room |
| **Objects** | Many objects get no lamp light, and stairs, railings and columns of one set light up differently | Furniture, stairs, railings, columns, trees and garden objects take the light of nearby lamps, evenly |
| **Walls and roofs** | Roofs stay black at night | Walls, foundations and roofs take lamp light, and walls and fences cast lamp shadows on the ground |
| **Weather** | Rain and snow surfaces miss lamp light in their own ways | Rain, snow and wet roads keep the right lamp light, and walked-on sidewalks stay visible under snow |
| **Dusk** | The ground isn't relit when lamps switch on, so a game loaded by day can keep dark lawns through the night | The ground relights when the lamps come on |

**While you build.** Move, add or delete a lamp in Build mode and the light follows right away: walls, floors, furniture and the ground update while you drag. This runs on your graphics card (**Lamp updates: Instant**, the default); on a weaker PC, **Classic** relights once you put the lamp down. Chain-link fences, lattices and railings let the light through their gaps, while brick walls block it.

**Make it your own.** The Lighting page has four tabs:

| Tab | What you set |
|---|---|
| **Overview** | The lighting balance (Subtle, Soft, Natural or your own) and how far lamps reach |
| **Outside** | Ground light, walls, roofs and objects |
| **Inside & floors** | Light between floors and rooms at night, including the background light of rooms with no lamp on |
| **Game fixes** | Corrections to the game's own lighting bugs, on by default; each one can be turned off |

[How the lighting works, in detail →](docs/features/night-lighting/README.md)

## Water reflections

| The Sims 3 | With Apex Radiance |
|---|---|
| Ponds and lakes reflect only a fixed picture of the sky. They never show the houses or trees around them, and at night they stay flat and dark. Some water in Twinbrook, Bridgeport and Moonlight Falls doesn't even show that. | Ponds and lakes mirror the trees, houses and lamps along their shore, by day and by night, in those three towns too. At night, lamps near the water also add glints and a soft glow on its surface. |

Reflection brightness and the lamp glow are set on the **Water & Snow** page.

> [!NOTE]
> Water Reflections needs Depth Blur or Ambient Occlusion turned on.

## Ambient Occlusion

| The Sims 3 | With Apex Radiance |
|---|---|
| No ambient occlusion: furniture looks like it floats above the floor, and rooms look flat. | Soft shadows where surfaces meet: under furniture, in room corners, where walls touch the floor and around houses and trees. |

The picture stays steady when the camera is still, with no shimmering, and lamp-lit surfaces keep their light and colour. Choose from five quality levels; the effect also works in map view.

**Sim Occlusion.** The shadow that looks good in a corner looks like dirt on a face. Apex recognises Sims by the game's Sim materials, custom content that uses them included, and gives their body and hair a lighter shade of their own. Transparent hair is covered too.

## Banding Fix

| The Sims 3 | With Apex Radiance |
|---|---|
| The world is drawn with only 256 shades per colour, so the soft pool of light a lamp throws on a wall breaks into visible rings, most of all in dark scenes and on OLED screens. | The 3D world is drawn with far more shades of colour and finished with a fine, invisible grain, so light fades smoothly across walls, floors, shadows and skies. |

Menus and the HUD aren't touched.

## Color, filters and LUTs

The Color page goes from small corrections to a completely new look for the game, and **it never touches the game's interface**: filters and colour changes apply to the 3D world only, so pie menus, the HUD, tooltips and plumbobs keep their own colours and stay sharp. Each group and each filter has its own switch, keeps its settings while off and costs nothing while off.

| Group | What it adjusts |
|---|---|
| **Basic** | Brightness, contrast, saturation, temperature and sharpness |
| **Tones** | Midtones, shadows, highlights and blacks |
| **Color** | Tint, vibrance, film-style toning of shadows and highlights, and a six-colour mixer (to calm the neon greens of plants, for example) |
| **Detail** | Clarity and a vignette |

**Filters.** 24 filters in six families. Turn on as many as you like and they add up:

| Family | Filters |
|---|---|
| Film looks | Technicolor 1 and 2, DPX Cineon, Vintage, Cross-process, Filmic pass, Black and white, Tint |
| Color and mood | Colorfulness, Night Mode, Levels |
| Light and detail | Auto exposure, Adaptive sharpening, Glow, Halation, Dreamy, Fake HDR |
| Camera | Emphasize, Tilt-shift, Prism, Film grain |
| Retro and style | 3DFX, CRT |
| Accessibility | Color-blind mode (red, green or blue colour blindness) |

**LUTs.** A LUT is a ready-made colour grade, the same kind photographers and film editors use. Put a 3D CUBE file (the format Photoshop, DaVinci Resolve and most LUT packs export) or a PNG strip (the layout ReShade LUTs use) in the Apex `LUTs` folder, pick it on the LUTs tab and set how strongly it applies. It works together with the filters and the adjustments.

> [!TIP]
> Right-click a filter to give it a keyboard shortcut. With the menu open, hold **B** to see the picture without your colour changes.

## Edge Smoothing and Depth Blur

**Neither touches the game's interface.** Both run before the game draws it, so pie menus, tooltips, plumbobs and the HUD stay sharp while the world behind them is smoothed or blurred.

| | The Sims 3 | With Apex Radiance |
|---|---|---|
| **Edges** | Jagged edges on roofs, fences, walls and furniture | **Edge Smoothing:** FXAA (lighter on your graphics card) or SMAA (cleaner long edges), with optional texture sharpening |
| **Distance** | Everything equally sharp | **Depth Blur:** the distant background softens like a camera lens focused on what is in front of it; lamps in the background stay bright, and the blur fades out in map view |

> [!TIP]
> Turn off the game's own Edge Smoothing under **Options > Graphics** when you use the Apex version. The menu points out game settings that get in the way of an effect.

## Performance

Fifteen optional switches make the game's heaviest work faster or spread it over several frames, without changing how the game looks. Turn each one on or off on its own and keep the ones that help on your PC.

| The Sims 3 | With Apex Radiance |
|---|---|
| Searches through around 290 game files for each thing it loads | Faster file and object lookups |
| Relights rooms and shades a newly loaded lot's walls all at once, even while the camera moves | Room lights settle sooner, and lot and wall updates are spread out while the camera moves |
| Compresses textures and caches slowly on a single thread, and sorts slowly in Create a Sim | Faster texture and cache compression, and faster sorting in Create a Sim |
| Its memory breaks into pieces in long sessions, until a save fails with Error 12 | **Room to save** keeps a block of memory free and hands it back right before each save, and memory handling is lighter |
| Redraws its window every frame, and its scripts call a library for every decimal comparison | Lighter window redraws and faster scripts |

**Lot Streaming** is experimental and off by default. It keeps lots detailed farther away and makes them appear more smoothly as you move.

> [!NOTE]
> Room to save makes Error 12 less likely, but no mod can rule it out or promise a particular FPS gain.

[All performance options →](docs/features/performance/README.md)

## Presets and the menu

**Presets.** Three built-in presets get you started: **Performance**, **Balanced** and **Quality**. A preset shows **Applied** while your settings still match it. Save your own looks, choose which settings a preset applies, give it a keyboard shortcut, and share it as a TOML file, or as a ZIP that includes its LUT. [More about presets →](docs/features/presets.md)

**The menu.** Every setting can be searched, undone or reset to its default. The Overview page lists every effect with one switch for all of them, and clicking the version number shows **What's new**. The menu is available in English, Portuguese, Spanish, French, German, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Czech, Polish, Hungarian, Greek, Russian, Japanese, Korean, Thai, Simplified Chinese and Traditional Chinese, and follows your Windows language until you pick one in Settings.

| Key | What it does |
|---|---|
| **Ctrl+Shift+F11** | Open the menu, once a world has loaded |
| **Ctrl+Shift+F10** | Turn Apex's effects off and on, to compare with the game |
| **Ctrl+Shift+F9** | Refresh the lighting |
| **F8** | Save a screenshot with Apex's effects |
| **F10** | Hide the game interface |
| **Ctrl+F** | Search the settings, with the menu open |
| **Alt** | Hold over the menu to see the game behind it |

Shortcuts can also use letters or numbers instead of F keys (**Settings > Shortcuts**).

## Installation

**You need**

- The Sims 3 for Windows: Steam **1.67.2** (`TS3W.exe`) or EA app **1.69** (`TS3.exe`). The menu tells you when a feature is unavailable on your game version.
- An ASI loader in the game's `Game\Bin` folder, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

**Install or update**

1. Close the game and the launcher.
2. Download `ApexRadiance.asi` from the [latest release](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest) or from [Nexus Mods](https://www.nexusmods.com/thesims3/mods/247).
3. Copy it into `The Sims 3\Game\Bin\`, replacing the old file when you update.
4. Load a world and press **Ctrl+Shift+F11** to open the menu.

> [!IMPORTANT]
> Always close the game before you copy, replace or remove the file.

Updating keeps your settings. Settings, presets, LUTs and reports live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\`: the main file is `ApexRadiance.toml`, presets are in `Profiles` and colour tables in `LUTs`.

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

These mods are recommended together with Apex Radiance:

| Mod | With Apex Radiance |
|---|---|
| **[Sims3SettingsSetter](https://github.com/sims3fiend/Sims3SettingsSetter/releases)** | Recommended. Apex detects its Split-Level Lighting Fix and doesn't apply the same fix twice. Don't use its experimental features together with Apex. |
| **[Sims 3 Modernization](https://github.com/rodneypantonial/sims3-modernization/releases)** | Recommended. Room to save recognises its save hook and still prepares the save before it. |
| **[Sims 3 Performance Patch](https://www.nexusmods.com/thesims3/mods/235)** | Recommended; the two mods change different parts of the game. With Sims 3 Modernization, turn off its `ShortWaitSpin` and `ThreadStackDiet`; Apex's **Settings > Compatibility** offers to do it for you. |
| **[DXVK](https://github.com/doitsujin/dxvk/releases)** | Supported, though results can vary between graphics backends. If you report a visual problem, tell us which one you use. |

**Settings > Compatibility** shows your game version, your platform and which of these mods are loaded.

## Reporting a problem

1. Open **Report a problem** in the Apex menu.
2. Save a report, record a short clip of the problem, capture the lighting on a surface or save a lighting snapshot. Each can have a title and a description.
3. Zip its folder from `Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\`.
4. Attach it to a [GitHub issue](https://github.com/loinyx/Sims3-ApexRadiance/issues) or post it in the Bugs tab on [Nexus Mods](https://www.nexusmods.com/thesims3/mods/247).

Tell us what happened, how to make it happen again, your game version, and whether you use DXVK or other graphics mods.

## Documentation and development

The [documentation](docs/README.md) explains every feature, its settings and how it was tested. The [release notes](docs/releases/README.md) list the changes in each version.

<details>
<summary><b>Building from source</b></summary>

Use Visual Studio 2022 Build Tools (v143), C++20 and the x86 Release target. The project uses a static CRT and vcpkg's `x86-windows-static` dependencies: Dear ImGui, Microsoft Detours and toml++.

Follow the [build workflow](docs/workflow.md#1-build) for dependency paths, public/developer flavours and validation requirements.

</details>

## Credits

- **Apex Radiance** by [@loinyx](https://github.com/loinyx).
- Sims3SettingsSetter by sims3fiend.
- Every-Story Ground Light (lamps on upper floors lighting the ground) uses a technique from [Arro](https://arro-now.tumblr.com/)'s Split-Level Lighting Fix.
- Lot Streaming and the daytime bloom fixes: research and code by [idavidveiga](https://github.com/idavidveiga).
- Edge Smoothing's FXAA mode follows FXAA 3.11 by Timothy Lottes (NVIDIA).
- Thanks to @boringbones for testing every build.

**Contributors:** [@rodneypantonial](https://github.com/rodneypantonial), scene update safety for effects and animal fur ([PR #38](https://github.com/loinyx/Sims3-ApexRadiance/pull/38)).

<details>
<summary><b>Third-party code</b></summary>

[Dear ImGui](https://github.com/ocornut/imgui) (MIT), [Microsoft Detours](https://github.com/microsoft/Detours) (MIT), [toml++](https://github.com/marzer/tomlplusplus) (MIT), [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al. (see `third_party/smaa/LICENSE.txt`), the texture sharpening after [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) (MIT), [Lucide](https://lucide.dev) icons (ISC, see `third_party/lucide/LICENSE`).

</details>

The Sims is a trademark of Electronic Arts Inc. This is a fan-made mod, not affiliated with or endorsed by Electronic Arts.

## License

[MIT](LICENSE)

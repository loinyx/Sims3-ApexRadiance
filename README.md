<p align="center"><img src="docs/images/logo-256.png" width="140" alt="Apex Radiance logo"></p>

# Apex Radiance for The Sims 3

**A brighter world. A night worth looking at.**

The Sims 3 builds its lighting out of separate pieces that don't talk to each other. The ground, each lot, every floor of a house, the objects, the roofs and the water all get their lamp light from a different system. That is why street light stops in a straight line at the edge of a lot, why a balcony lamp leaves the porch below it dark, and why roofs and ponds turn black at night.

Apex Radiance rebuilds that lighting so it works as one. A lamp lights everything around it: the floor and walls of its room, the furniture, the floors above and below, the lawn, the street and the lot next door. The mod also adds graphics effects the game never had, such as ambient occlusion, water reflections and colour filters, along with optional fixes for stutters and memory.

**[Download the latest version](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest)** · [Installation](#installation) · [Release notes](https://github.com/loinyx/Sims3-ApexRadiance/releases) · [Report a problem](#reporting-a-problem)

## See it in game

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

| | |
|---|---|
| **[Lighting](#lighting)** | Lamp light that reaches the ground, the walls, every floor and every object, without the game's cuts and blocks |
| **[Water reflections](#water-reflections)** | Ponds and lakes mirror the houses, trees and lamps on their shore, by day and by night |
| **[Ambient Occlusion](#ambient-occlusion)** | Soft contact shadows under furniture and in corners, with separate control for Sims |
| **[Banding Fix](#banding-fix)** | Smooth light on walls and in the sky instead of rings |
| **[Color, filters and LUTs](#color-filters-and-luts)** | Small corrections or a completely new look for the game |
| **[Edge Smoothing and Depth Blur](#edge-smoothing-and-depth-blur)** | Clean edges and a camera-like blur in the distance |
| **[Performance](#performance)** | Optional fixes for stutters and for saves that fail with Error 12 |

Everything is set from one in-game menu, available in 21 languages.

## Lighting

### What's wrong in the base game

- Street lamps stop in a straight line at the edge of a lot, and lamps on a lot never reach the street.
- The light on the ground is drawn in coarse 1-metre blocks with coloured specks.
- Each floor of a house only sees its own lamps, so light cuts off at the floor line and a lamp upstairs never reaches the ground.
- Many objects get no lamp light at all, and stairs, railings and columns from the same set light up differently.
- Roofs and ponds get no lamp light at all, so they turn black at night.
- The ground isn't relit when lamps switch on at dusk, so a game loaded during the day can keep dark lawns through the night.

### What Apex Radiance changes

**Outside**

- Street lamps light the lots next to them, and lot lamps light the grass, roads and sidewalks past the lot edge.
- The ground gets soft, round pools of light, on grass, roads, sidewalks and patios alike.
- Walls cast lamp shadows on the ground. Chain-link fences, lattices and railings let the light through their gaps; brick walls block it.
- Walls, foundations, roofs, trees and garden objects take the light of the lamps near them. Spotlights and lamps with shades light the lawn through their cone.
- Rain, snow and wet roads keep the right lamp light, and sidewalks where Sims have walked stay visible under the snow.

**Inside and between floors**

- Lamps on balconies and upper floors light the ground below them.
- Indoor light reaches the floors above and below through stairwells and open floors, and stops at solid floors.
- Light passes through doors, windows and arches into the next room and fades across it.
- Furniture, stairs, railings and columns light up evenly, and walls meet between floors without seams.
- Rooms with no lamp on keep a soft background light that you can adjust.

**While you build**

Move, add or delete a lamp in Build mode and the light follows right away: walls, floors, furniture and the ground update while you drag. This runs on your graphics card (**Lamp updates: Instant**, the default). On a weaker PC, **Classic** relights once you put the lamp down.

### Make it your own

The Lighting page has four tabs:

- **Overview:** pick a lighting balance (Subtle, Soft, Natural or your own) and how far lamps reach.
- **Outside:** ground light, walls, roofs and objects.
- **Inside & floors:** light between floors and rooms at night.
- **Game fixes:** the corrections to the game's own lighting bugs. They are on by default, and each one can be turned off.

[How the lighting works, in detail →](docs/features/night-lighting/README.md)

## Water reflections

**In the base game,** ponds and lakes reflect only a fixed picture of the sky. They never show the houses or trees around them, and at night they stay flat and dark. Some water in Twinbrook, Bridgeport and Moonlight Falls doesn't even show that.

**With Apex Radiance,** ponds and lakes mirror the trees, houses and lamps along their shore, by day and by night, those three towns included. At night, lamps near the water also add glints and a soft glow on its surface. You can adjust both on the **Water & Snow** page. Water Reflections needs Depth Blur or Ambient Occlusion turned on.

## Ambient Occlusion

**In the base game,** there is no ambient occlusion. Furniture looks like it floats above the floor, and rooms look flat.

**With Apex Radiance,** soft shadows appear where surfaces meet: under furniture, in room corners, where walls touch the floor and around houses and trees. The picture stays steady when the camera is still, with no shimmering, and lamp-lit surfaces keep their light and colour. Choose from five quality levels; the effect also works in map view.

**Sim Occlusion** treats Sims on their own. The shadow that looks good in a corner looks like dirt on a face, so Apex recognises Sims (custom content included, as long as it uses the game's Sim materials) and gives their body and hair a lighter shade of their own. Transparent hair is covered too.

## Banding Fix

**In the base game,** the world is drawn with only 256 shades per colour, so the soft pool of light a lamp throws on a wall breaks into visible rings. It shows most in dark scenes and on OLED screens.

**With Apex Radiance,** the 3D world is drawn with far more shades of colour and finished with a fine, invisible grain, so light fades smoothly across walls, floors, shadows and skies. Menus and the HUD aren't touched.

## Color, filters and LUTs

The Color page goes from small corrections to a completely new look for the game. It only changes the 3D world, so menus and the HUD keep their colours. Each group and each filter has its own switch, keeps its settings while off and costs nothing while off.

**Adjustments.** Brightness, contrast, saturation and temperature; midtones, shadows, highlights and blacks; tint, vibrance and film-style toning of shadows and highlights; a six-colour mixer (to calm the neon greens of plants, for example); clarity and a vignette.

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

Right-click a filter to give it a keyboard shortcut. With the menu open, hold **B** to see the picture without your colour changes.

## Edge Smoothing and Depth Blur

**Edge Smoothing** removes the jagged edges of roofs, fences, walls and furniture. Choose FXAA (lighter on your graphics card) or SMAA (cleaner long edges), with optional texture sharpening. Turn off the game's own Edge Smoothing under **Options > Graphics** when you use it.

**Depth Blur** softens the distant background like a camera lens focused on what is in front of it. Lamps in the background stay bright, and the blur fades out in map view.

Both leave the game's interface sharp: pie menus, tooltips and the HUD are drawn after them.

## Performance

**In the base game,** many pauses come from work done all at once: searching through around 290 game files for each thing it loads, relighting every room of a lot in one go, compressing textures slowly on a single thread, placing a whole lot's objects in one frame. In long sessions its memory also breaks into pieces until a save fails with Error 12.

**With Apex Radiance,** fifteen optional switches make that work faster or spread it over several frames, without changing how the game looks. Turn each one on or off on its own and keep the ones that help on your PC. **Room to save** keeps a block of memory free and hands it back right before each save, which makes Error 12 less likely, though no mod can rule it out.

**Lot Streaming** is experimental and off by default. It keeps lots detailed farther away and makes them appear more smoothly as you move.

[All performance options →](docs/features/performance/README.md)

## Presets

Three built-in presets get you started: **Performance**, **Balanced** and **Quality**. A preset shows **Applied** while your settings still match it. Save your own looks, choose which settings a preset applies, give it a keyboard shortcut, and share it as a TOML file, or as a ZIP that includes its LUT.

[More about presets →](docs/features/presets.md)

## The menu

| Key | What it does |
|---|---|
| **Ctrl+Shift+F11** | Open the menu, once a world has loaded |
| **Ctrl+Shift+F10** | Turn Apex's effects off and on, to compare with the game |
| **Ctrl+Shift+F9** | Refresh the lighting |
| **F8** | Save a screenshot with Apex's effects |
| **F10** | Hide the game interface |
| **Ctrl+F** (menu open) | Search the settings |
| **Alt** (held over the menu) | See the game behind the menu |

Every setting can be undone or reset to its default. The Overview page lists every effect, with one switch for all of them, and clicking the version number shows **What's new**. Shortcuts can use F keys, letters or numbers.

The menu is available in English, Portuguese, Spanish, French, German, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Czech, Polish, Hungarian, Greek, Russian, Japanese, Korean, Thai, Simplified Chinese and Traditional Chinese. It follows your Windows language until you pick one in Settings.

## Installation

### Requirements

- The Sims 3 for Windows: Steam **1.67.2** (`TS3W.exe`) or EA app **1.69** (`TS3.exe`). The menu tells you when a feature is unavailable on your game version.
- An ASI loader in the game's `Game\Bin` folder, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

### Install or update

1. Close the game and the launcher.
2. Download `ApexRadiance.asi` from the [latest release](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest).
3. Copy it into `The Sims 3\Game\Bin\`, replacing the old file when you update.
4. Load a world and press **Ctrl+Shift+F11** to open the menu.

Updating keeps your settings. Settings, presets, LUTs and reports live here:

```text
Documents\Electronic Arts\The Sims 3\Apex Radiance\
```

The main settings file is `ApexRadiance.toml`; presets are in `Profiles` and colour tables in `LUTs`.

**Steam Deck and Linux:** Apex runs through Proton with the same file. Add the loader's DLL override to the game's launch options in Steam ([how to](docs/features/steam-deck.md#installing-on-steam-deck-or-linux)).

**Something looks broken?** **Settings → Compatibility → Recovery → Restart Apex** rebuilds the image effects without touching your settings.

**Coming from an older combined build?** Remove the old combined ASI or `S3SSApex.asi` from `Game\Bin`, and keep the official standalone `Sims3SettingsSetter.asi` if you use it. Apex imports your old settings on its first start.

## Compatibility

These mods are recommended together with Apex Radiance:

- **[Sims3SettingsSetter](https://github.com/sims3fiend/Sims3SettingsSetter/releases):** works alongside Apex, which detects its Split-Level Lighting Fix and doesn't apply the same fix twice. Don't use its experimental features together with Apex.
- **[Sims 3 Modernization](https://github.com/rodneypantonial/sims3-modernization/releases):** works alongside Apex. Room to save recognises its save hook and still prepares the save before it.
- **[Sims 3 Performance Patch](https://www.nexusmods.com/thesims3/mods/235):** works alongside Apex; the two mods change different parts of the game. With Sims 3 Modernization, turn off its `ShortWaitSpin` and `ThreadStackDiet`; Apex's **Settings > Compatibility** offers to do it for you.
- **[DXVK](https://github.com/doitsujin/dxvk/releases):** supported, though results can vary between graphics backends. If you report a visual problem, tell us which one you use.

**Settings > Compatibility** shows your game version, your platform and which of these mods are loaded.

## Reporting a problem

If something looks wrong, open **Report a problem** in the Apex menu. You can save a report, record a short clip of the problem, capture the lighting on a surface or save a lighting snapshot, each with an optional title and description.

Reports are saved in dated folders under:

```text
Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\
```

Zip the folder and attach it to a [GitHub issue](https://github.com/loinyx/Sims3-ApexRadiance/issues), or post it in the mod's Bugs tab on Nexus Mods. Tell us what happened, how to make it happen again, your game version, and whether you use DXVK or other graphics mods.

## Documentation and development

The [documentation](docs/README.md) explains every feature, its settings and how it was tested. The [release notes](docs/releases/README.md) list the changes in each version.

<details>
<summary>Building from source</summary>

Use Visual Studio 2022 Build Tools (v143), C++20 and the x86 Release target. The project uses a static CRT and vcpkg's `x86-windows-static` dependencies: Dear ImGui, Microsoft Detours and toml++.

Follow the [build workflow](docs/workflow.md#1-build) for dependency paths, public/developer flavours and validation requirements.

</details>

## Credits

- **Apex Radiance** by [@loinyx](https://github.com/loinyx).
- Sims3SettingsSetter by sims3fiend
- Every-Story Ground Light (lamps on upper floors lighting the ground) uses a technique from [Arro](https://arro-now.tumblr.com/)'s Split-Level Lighting Fix.
- Lot Streaming and the daytime bloom fixes: research and code by [idavidveiga](https://github.com/idavidveiga).
- Edge Smoothing's FXAA mode follows FXAA 3.11 by Timothy Lottes (NVIDIA).
- Thanks to @boringbones for testing every build.
- Third-party code: [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [Microsoft Detours](https://github.com/microsoft/Detours) (MIT), [toml++](https://github.com/marzer/tomlplusplus) (MIT), [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al. (see `third_party/smaa/LICENSE.txt`), the texture sharpening after [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) (MIT), [Lucide](https://lucide.dev) icons (ISC, see `third_party/lucide/LICENSE`).

### Contributors

- [@rodneypantonial](https://github.com/rodneypantonial): scene update safety for effects and animal fur ([PR #38](https://github.com/loinyx/Sims3-ApexRadiance/pull/38)).

The Sims is a trademark of Electronic Arts Inc. This is a fan-made mod, not affiliated with or endorsed by Electronic Arts.

## License

[MIT](LICENSE)

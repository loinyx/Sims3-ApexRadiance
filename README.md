<p align="center"><img src="docs/images/logo-256.png" width="140" alt="Apex Radiance logo"></p>

# Apex Radiance for The Sims 3

**A brighter world. A night worth looking at.**

Apex Radiance changes how The Sims 3 lights the world after dark. Lamps light the lawn, the street and the lot next door, the walls beside them, the floors above and below, and the rooms behind doors and windows. Move a lamp in Build mode and its light follows it while you drag. Around the lighting sit image effects (ambient occlusion, edge smoothing, depth blur, banding fix, colour grading with filters and LUTs), fifteen optional fixes for stutters and one in-game menu in 21 languages.

**[Download the latest version](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest)** · [Installation](#installation) · [Release notes](https://github.com/loinyx/Sims3-ApexRadiance/releases) · [Report a problem](#reporting-a-problem)

## See the lighting

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

## Night Lights

The game bakes lamp light into coarse maps that stop at lot borders, leave out upper floors and turn blocky up close. Apex Radiance draws that light again where the game leaves it out.

### Outside

- **Across lot borders:** street lamps light the lots beside them, and lot lamps light the grass, roads and sidewalks past the lot edge.
- **Smooth ground light:** grass, roads, sidewalks and patios get soft pools of light instead of 1 m blocks.
- **Lamp shadows on the ground:** walls shade the lamp light near them; chain-link fences, lattices and railings let it through their gaps, brick walls block it.
- **Buildings and objects:** walls, foundations, roofs, furniture, trees and garden details take the light of nearby lamps. Spotlights and shaded lamps light the lawn through their cones.
- **Weather:** rain, snow and wet roads keep the right lamp light, and walked-on sidewalks stay visible under snow.

### Inside and between floors

- **Every story:** lamps on balconies and upper floors light the ground below; indoor lamps reach the floors above and below through stairwells and open floors, while solid floors block them.
- **Doors, windows and arches:** lamp light passes through openings into the next room and fades across it. Doors can block it.
- **Rooms at night:** set the background light and tint of rooms with no lamp on.
- **No blocks or rings:** room light is smoothed, wall lamps light the wall at their own height and walls meet between floors without seams.

### Moving lamps

With **Lamp updates: Instant (GPU)**, the default, the active lot's outdoor lamps are drawn on the graphics card with shadows. Drag a lamp, add one or delete one, and walls, floors, furniture, fences and the ground follow while you move it. **Classic** is easier on the graphics card and relights once you put the lamp down.

### Make it your own

The Lighting page has four tabs. **Overview** sets the balance (Subtle, Soft, Natural or Custom) and how far lamps reach. **Outside** and **Inside & floors** hold the look choices: one Ground light slider, walls, roofs, objects, light between stories and rooms at night. **Game fixes** gathers the corrections to the game's own lighting bugs; they are on by default and each one can be switched off.

[Read how the lighting works →](docs/features/night-lighting/README.md)

## Water and snow

At night, lamps light ponds and lakes and glow softly around their bulbs. **Water Reflections** brings back reflections on world water, Twinbrook, Bridgeport and Moonlight Falls included, and needs Depth Blur or Ambient Occlusion. The **Water & Snow** page keeps these controls together.

## Image effects

- **Color:** brightness, contrast, saturation, temperature, tones, split toning, a colour mixer, clarity and a vignette. Each group has its own switch and keeps its values while off.
- **Filters and LUTs:** 24 stackable filters, such as Technicolor, DPX Cineon, Vintage, Cross-process, black and white, Night Mode, Glow, Halation, Film grain, CRT and a colour-blind mode, plus PNG and 3D CUBE LUTs from the Apex `LUTs` folder. Right-click a filter to give it a keyboard shortcut.
- **Ambient Occlusion:** soft contact shadows where surfaces meet, at five quality levels, also in map view. **Sim Occlusion** has separate controls for bodies and hair.
- **Edge Smoothing:** SMAA or FXAA for jagged roofs, fences, walls and furniture, with optional texture sharpening.
- **Depth Blur:** softens the scenery outside your focus and fades out in map view.
- **Banding Fix:** smooths the visible steps in skies, shadows and glows; its High-precision scene is on by default.

Use the before/after controls to compare with the game's own image. For the depth-based effects, turn off the game's **Edge Smoothing** under **Options > Graphics** and use the Apex version; the menu points out combinations that need attention.

## Presets

Three built-in presets, **Performance**, **Balanced** and **Quality**, sit under your own saved looks, and a preset shows **Applied** while your setup still matches it. Save, import and export presets as TOML, or as one ZIP with their LUT, choose which settings a preset applies, and give a preset a keyboard shortcut.

[More about presets →](docs/features/presets.md)

## Performance

Fifteen optional switches go after stutters: room lights that settle sooner, lot and wall updates spread out while the camera moves, fewer repeated file and object lookups, faster texture and cache compression, faster sorting in Create a Sim, better memory handling, lighter window redraws and faster scripts. **Room to save** keeps room for a save when memory runs low. Each one switches on and off on its own, so keep the ones that help on your PC.

**Lot Streaming** is experimental and off by default: it keeps lots detailed farther away and smooths how they appear as you move.

[Explore the performance options →](docs/features/performance/README.md)

## The menu

Once a world has loaded, press **Ctrl+Shift+F11** to open the menu. Search any setting, undo a change or reset it to its default, and hold **Alt** over the menu to see the game behind it. The Overview page lists every effect on one card, with one switch for all of them. Click the version number to read **What's new**.

Press **F8** to save a screenshot with Apex effects; choose whether the game interface shows and where the file goes. **F10** hides the interface while you play. Shortcuts can use letters, numbers or F keys.

**Menu languages (21):** English, Portuguese, Spanish, French, German, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Czech, Polish, Hungarian, Greek, Russian, Japanese, Korean, Thai, Simplified Chinese and Traditional Chinese. The menu follows your Windows language until you pick one in Settings.

## Installation

### Requirements

- The Sims 3 for Windows: Steam **1.67.2** (`TS3W.exe`) or EA app **1.69** (`TS3.exe`). The menu reports when a feature is unavailable on your game version.
- An ASI loader in the game's `Game\Bin` folder, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

### Install or update

1. Close the game and launcher.
2. Download `ApexRadiance.asi` from the [latest release](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest).
3. Copy it into `The Sims 3\Game\Bin\`, replacing the previous Apex Radiance file when updating.
4. Load a world and press **Ctrl+Shift+F11** to open the menu.

Updating Apex won't remove your settings. Settings, presets, LUTs and reports are stored here:

```text
Documents\Electronic Arts\The Sims 3\Apex Radiance\
```

The main settings file is `ApexRadiance.toml`. Saved presets stay in `Profiles`, and colour tables in `LUTs`.

**Settings → Compatibility → Recovery → Restart Apex** recreates the image-effect resources while keeping your settings. It does not uninstall the mod or reset lighting hooks. Close the game before replacing or removing the ASI.

**Steam Deck and Linux:** Apex runs through Proton with the same ASI. Add the loader's DLL override to the game's launch options in Steam ([how to](docs/features/steam-deck.md#installing-on-steam-deck-or-linux)).

**Upgrading from an older combined build?** Remove the old combined ASI or `S3SSApex.asi` from `Game\Bin`. Keep the official standalone `Sims3SettingsSetter.asi` if you use it. Apex imports recognised legacy settings on its first start.

## Compatibility

These mods are recommended together with Apex Radiance:

- **[Sims3SettingsSetter](https://github.com/sims3fiend/Sims3SettingsSetter/releases):** works alongside Apex. Apex detects its active Split-Level Lighting Fix and avoids applying the same fix twice. Don't use its experimental features together with Apex.
- **[Sims 3 Modernization](https://github.com/rodneypantonial/sims3-modernization/releases):** works alongside Apex. Room to save recognises its save hook and still prepares the save before it.
- **[Sims 3 Performance Patch](https://www.nexusmods.com/thesims3/mods/235):** works alongside Apex; the mods target different game operations. With Sims 3 Modernization, turn off its `ShortWaitSpin` and `ThreadStackDiet`: Apex's Settings > Compatibility offers to do it.
- **[DXVK](https://github.com/doitsujin/dxvk/releases):** supported, though results can vary between graphics backends. If you report a visual issue, let us know which backend you use.

Settings > Compatibility shows your game version, your platform and which of these mods are loaded. Room to save and the memory options make Error 12 less likely, but they cannot rule it out or promise a particular FPS gain.

## Reporting a problem

If something looks wrong, open **Report a problem** in the Apex menu. You can save a report, record a short reproduction, capture lighting on a surface, or save a lighting snapshot. Add a title and description to a capture if you like.

Reports are stored in dated folders under:

```text
Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\
```

Zip the folder for the issue and attach it to a [GitHub issue](https://github.com/loinyx/Sims3-ApexRadiance/issues) or post it in the mod's Bugs tab on Nexus Mods. Please describe what happened, how to reproduce it, your game version, and whether you use DXVK or other graphics mods.

## Documentation and development

The [documentation index](docs/README.md) covers feature settings, compatibility, engine research and validation. [Release notes](docs/releases/README.md) describe changes between versions.

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

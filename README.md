<p align="center"><img src="docs/images/logo-256.png" width="140" alt="Apex Radiance logo"></p>

# Apex Radiance for The Sims 3

**A brighter world. A night worth looking at.**

Bring your Sims' neighbourhoods to life after dark. Apex Radiance changes how The Sims 3 calculates lamp lighting, helping light reach nearby roads and terrain, walls, upper floors and objects. Streetlights can cast light across lot borders, balcony lights can reach the ground below, and supported water and snow respond to nearby lamps. The in-game menu also brings together lighting controls, colour adjustments, film-inspired filters, softer shadows, camera effects and optional performance tweaks.

**Supported menu languages (21):** English, Portuguese, Spanish, French, German, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Czech, Polish, Hungarian, Greek, Russian, Japanese, Korean, Thai, Simplified Chinese and Traditional Chinese. Select a language in Settings or let the menu follow your Windows language.

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

## Lighting that reaches your world

Lighting is the main focus of Apex Radiance. Streetlights can spill over lot borders, lot lamps can brighten nearby roads and terrain, and lights around the house can reach walls, upper floors and outdoor details.

### Streets, terrain and lots

- **Light across lot borders:** street lamps illuminate nearby lots, and lot lamps contribute to the surrounding terrain and roads.
- **Smoother ground lighting:** grass, sidewalks, streets and lot floors receive softer pools of lamp light.
- **Lamps on upper floors:** balcony and terrace lamps can illuminate the ground below.
- **Weather and snow:** lighting corrections cover snowy surfaces and rain/snow variants; walked-on sidewalks remain visible through snow.

### Houses, rooms and objects

- **Walls, foundations and roofs:** exterior surfaces receive nearby lamp light, including walls above ground-floor lamps and surfaces around balconies.
- **Openings and enclosed spaces:** walls block outdoor lamp light, while the lighting fix accounts for doors and windows around enclosed yards.
- **Light between stories:** indoor lamps illuminate through stairwells and open floors, while solid floors and walls block their light.
- **Objects and foliage:** nearby lamp light brings gardens, fences, furniture and outdoor details into the scene.
- **Windows:** outward-facing window surfaces can receive the light of outdoor lamps.

### Make the night your own

Pick **Subtle**, **Soft** or **Natural**, then adjust how bright the ground, roads, objects, buildings and other surfaces should be. The Lighting page also lets you change lamp colour, moonlight and the background light in unlit rooms.

[Read how the lighting works →](docs/features/night-lighting/README.md)

## Water, reflections and snow

At night, lamps can light up ponds and lakes, and supported shorelines can reflect nearby trees and buildings. Reflections on world water are also restored in Twinbrook, Bridgeport and Moonlight Falls. On snowy lots, walked-on sidewalks remain visible. The **Water & Snow** page keeps these controls together.

## Colour and image effects

Use the Color page to adjust the image to your taste. Its controls are grouped by what they affect, and you can switch each group off without losing its saved settings.

### Color

Start from **Overview** to see the image groups at a glance. The main Image switch turns off all colour adjustments together; each group's switch can also be used on its own, without erasing its saved values.

- **Basic:** brightness, contrast, saturation, temperature and sharpness.
- **Tones:** control midtones, shadows, highlights and black levels.
- **Color:** shape the mood with split tones, a six-colour mixer and a vignette.
- **Detail:** adjust clarity, sharpness and darker corners.
- **Filters:** layer 25 looks such as Technicolor, DPX Cineon, Vintage, Cross-process, black and white, Night Mode, LUT, Auto exposure, Glow, Halation, Film grain, CRT and colour-blind mode. Each has a separate switch, strength and expandable controls.

Filters are off by default. To set a shortcut, right-click a filter and choose the key combination you want to use to toggle it. Shortcuts are never assigned automatically; once set, yours appears beside the filter. You can also load PNG LUTs from the Apex `LUTs` folder.

Use the before/after controls to compare your adjustments with the game's original image.

### Ambient Occlusion and Sims

Ambient Occlusion adds soft contact shadows under furniture, in corners, around buildings and where surfaces meet. Choose one of five quality levels, then adjust its strength and distance. Temporal smoothing helps keep the shadows steady as you move the camera, while Thin Object Detail helps them show up around legs, rails and other narrow objects. AO also works in map view.

**Sim Occlusion** has separate controls for the body and hair. You can limit how dark the added shading gets and enable support for compatible transparent hair, softening shadows on Sims while keeping the room's contact shadows.

### Depth Blur, Edge Smoothing and Banding Fix

- **Depth Blur** softens scenery outside the area you focus on. Adjust the focus to get the look you want; the effect fades out in map view.
- **Edge Smoothing** offers FXAA and SMAA to clean up jagged roof, fence, wall and furniture edges. Optional sharpening restores fine texture detail.
- **Banding Fix** helps smooth visible steps in gradual light and shadow, with an optional setting for smoother sky gradients.

For the best compatibility with depth-based effects, turn off the game's **Edge Smoothing** under **Options > Graphics** and use the Apex version. The menu will point out combinations that may need attention.

## Performance options

If small pauses are interrupting play, Apex includes **15 optional performance settings** you can try. Each one can be toggled on its own:

- **Camera and lighting:** room lights settle sooner; lot and wall updates are spread out while the camera moves; and new objects are processed in smaller batches.
- **Files and objects:** cut down on repeated searches for game files, missing files and objects, and build file lists faster.
- **Textures and Sims:** speed up texture and cache compression, as well as sorting while Sims are being created.
- **Memory:** improve memory handling and use **Room to save**, which reserves space for a save when memory runs low.
- **Game and scripts:** reduce unnecessary window redraws and speed up selected script operations.

Every setting is optional. Keep the ones that help on your system and turn off any that don't.

Experimental options are clearly marked. **Lot Streaming** is off by default; if you turn it on, you can adjust how far away lots stay detailed and how they appear as you move.

[Explore the performance options →](docs/features/performance/README.md)

## Screenshots and the in-game menu

Once a world has loaded, press **Ctrl+Shift+F11** to open the menu. From there, you can search settings, save or apply profiles, choose which profile sections to use, undo a change, or restore a setting to its default. Hold **Alt** over the menu to see the game behind it, or click the version number to read **What's new**.

Press **F8** to save a screenshot with Apex effects applied. You can choose whether the game's interface appears in the picture and where the file is saved. Press **F10** to hide the interface while you play.

## Installation

### Requirements

- The Sims 3 for Windows: Steam **1.67.2** (`TS3W.exe`) or EA app **1.69** (`TS3.exe`). The menu reports when a feature is unavailable on your game version.
- An ASI loader in the game's `Game\Bin` folder, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

### Install or update

1. Close the game and launcher.
2. Download `ApexRadiance.asi` from the [latest release](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest).
3. Copy it into `The Sims 3\Game\Bin\`, replacing the previous Apex Radiance file when updating.
4. Load a world and press **Ctrl+Shift+F11** to open the menu.

Updating Apex won't remove your settings. Settings, profiles, LUTs and reports are stored here:

```text
Documents\Electronic Arts\The Sims 3\Apex Radiance\
```

The main settings file is `ApexRadiance.toml`.

**Upgrading from an older combined build?** Remove the old combined ASI or `S3SSApex.asi` from `Game\Bin`. Keep the official standalone `Sims3SettingsSetter.asi` if you use it. Apex imports recognised legacy settings on its first start.

## Compatibility

- **Sims3SettingsSetter:** works alongside Apex. Apex detects its active Split-Level Lighting Fix and avoids applying the same fix twice.
- **Sims 3 Performance Patch:** can run alongside Apex; the mods target different game operations.
- **DXVK:** supported, though results can vary between graphics backends. If you report a visual issue, let us know which backend you use.

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
- Third-party code: [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [Microsoft Detours](https://github.com/microsoft/Detours) (MIT), [toml++](https://github.com/marzer/tomlplusplus) (MIT), [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al. (see `third_party/smaa/LICENSE.txt`), the texture sharpening after [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) (MIT), [Lucide](https://lucide.dev) icons (ISC, see `third_party/lucide/LICENSE`).

The Sims is a trademark of Electronic Arts Inc. This is a fan-made mod, not affiliated with or endorsed by Electronic Arts.

## License

[MIT](LICENSE)

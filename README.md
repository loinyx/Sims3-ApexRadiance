<p align="center"><img src="docs/images/logo-256.png" width="140" alt="Apex Radiance logo"></p>

# Apex Radiance for The Sims 3

**A brighter world. A night worth looking at.**

Apex Radiance is more than an image filter. It works in The Sims 3’s lighting itself, correcting how lamp light is calculated and where it reaches: across terrain and lot borders, onto walls and upper floors, and around objects in the world. See neighbourhoods, homes and gardens come alive after dark, with reflections and lighting that respond to the scene. Colour controls, film-inspired filters, softer shadows and camera effects let you shape the final look, while optional performance improvements help reduce everyday stutters—all from one in-game menu.

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

Lighting is the heart of Apex Radiance. Lamps do more than glow: they illuminate the streets, homes and surroundings that make your neighbourhood feel alive.

### Streets, terrain and lots

- **Light across lot borders:** street lamps illuminate nearby lots, and lot lamps contribute to the surrounding terrain and roads.
- **Smoother ground lighting:** grass, sidewalks, streets and lot floors receive softer pools of lamp light.
- **Lamps on upper floors:** balcony and terrace lamps can illuminate the ground below.
- **Weather and snow:** lighting corrections cover snowy surfaces and rain/snow variants; walked-on sidewalks remain visible through snow.

### Houses, rooms and objects

- **Walls, foundations and roofs:** exterior surfaces receive nearby lamp light, including walls above ground-floor lamps and surfaces around balconies.
- **Openings and enclosed spaces:** walls block outdoor lamp light; doors and windows are accounted for when light reaches enclosed yards.
- **Light between stories:** indoor lamps illuminate through stairwells and open floors, while solid floors and walls block their light.
- **Objects and foliage:** nearby lamp light brings gardens, fences, furniture and outdoor details into the scene.
- **Windows:** outward-facing window surfaces can receive the light of outdoor lamps.

### Make the night your own

Choose **Subtle**, **Soft** or **Natural**, then fine-tune brightness for the ground, roads, objects, buildings and other surfaces. Adjust lamp colour, moonlight and the background light in unlit rooms from the Lighting page.

[Read how the lighting works →](docs/features/night-lighting/README.md)

## Water, reflections and snow

Night lighting continues onto water and winter surfaces. Lamps add glow to ponds and lakes, while supported shores reflect nearby trees and buildings. Reflections are also restored on world water in Twinbrook, Bridgeport and Moonlight Falls. Walked-on sidewalks show through snow. **Water & Snow** keeps these controls together.

## Colour and image effects

The image tools let you tune the finished scene to match the atmosphere you want. Each area has its own controls and can be switched separately.

### Color

Start from **Overview** to see the image groups at a glance. The main Image switch turns off all colour adjustments together; each group's switch can also be used on its own, without erasing its saved values.

- **Basic:** brightness, contrast, saturation, temperature and sharpness.
- **Tones:** control midtones, shadows, highlights and black levels.
- **Color:** shape the mood with split tones, a six-colour mixer and a vignette.
- **Detail:** adjust clarity, sharpness and darker corners.
- **Filters:** layer 25 looks such as Technicolor, DPX Cineon, Vintage, Cross-process, black and white, Night Mode, LUT, Auto exposure, Glow, Halation, Film grain, CRT and colour-blind mode. Each has a separate switch, strength and expandable controls.

Filters are off by default. Right-click any filter to assign an optional keyboard combination to turn it on or off. The shortcut tag sits to the left of its expand arrow. Shortcuts are never assigned automatically. You can also load PNG LUTs from the Apex `LUTs` folder.

Use the before/after controls to compare your adjustments with the game's original image.

### Ambient Occlusion and Sims

Ambient Occlusion adds soft contact shade beneath furniture, in corners, around buildings and where surfaces meet. Choose among five quality levels and tune its strength, reach and distance. Temporal smoothing steadies the shade as the view moves, and Thin object detail helps it appear around legs, rails and other narrow objects. AO also works in map view.

**Sim Occlusion** controls how that shade appears on Sims: tune the body and hair separately, limit how dark Sims can become, and enable support for compatible transparent hair. That lets you soften facial and hair shadows while keeping the room's contact shade.

### Depth Blur, Edge Smoothing and Banding Fix

- **Depth Blur** softens distant scenery while keeping the area in focus clear. Adjust focus to get the look you want; the effect fades out in map view.
- **Edge Smoothing** offers FXAA and SMAA to clean up jagged roof, fence, wall and furniture edges. Optional sharpening restores fine texture detail.
- **Banding Fix** helps smooth visible steps in gradual light and shadow, with an optional setting for smoother sky gradients.

For the best compatibility with depth-based effects, turn off the game's own **Edge Smoothing** in **Options > Graphics** and use Apex's version. The menu explains when a combination needs attention.

## Performance options

Alongside the visual features, Apex provides **15 individual performance controls** for small pauses that can interrupt play:

- **Camera and lighting:** room lights settle sooner, lot and wall updates spread out while the camera moves, and new objects appear in smaller batches.
- **Files and objects:** fewer repeated searches for game files, missing files and objects; faster file lists.
- **Textures and Sims:** faster texture and cache compression, plus faster sorting while Sims are created.
- **Memory:** quicker memory handling and **Room to save**, which reserves space for saving when memory runs low.
- **Game and scripts:** fewer window redraws and faster selected script operations.

Each control can be switched individually. Keep the options that help your game and turn off any that do not suit your setup.

Options marked experimental are identified in the menu. **Lot Streaming**, off by default, keeps more lots detailed farther from the camera and offers controls for detail distance and how lots appear as you move.

[Explore the performance options →](docs/features/performance/README.md)

## Screenshots and the in-game menu

Open the menu with **Ctrl+Shift+F11** after loading a world. Search the settings, apply or save profiles, choose which profile sections to use, undo a change or restore a control to its default. Hold **Alt** over the menu to look at the game behind it. Click the version at the bottom to read **What's new**.

Press **F8** to save a screenshot with Apex effects applied. Choose whether to hide the game's interface in the photo and where to save it. **F10** hides the game interface during play.

## Installation

### Requirements

- The Sims 3 for Windows: Steam **1.67.2** (`TS3W.exe`) or EA app **1.69** (`TS3.exe`). The menu reports when a feature is unavailable on your game version.
- An ASI loader in the game's `Game\Bin` folder, such as [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

### Install or update

1. Close the game and launcher.
2. Download `ApexRadiance.asi` from the [latest release](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest).
3. Copy it into `The Sims 3\Game\Bin\`, replacing the previous Apex Radiance file when updating.
4. Load a world and press **Ctrl+Shift+F11** to open the menu.

Your settings are kept when updating. Settings, profiles, LUTs and reports live under:

```text
Documents\Electronic Arts\The Sims 3\Apex Radiance\
```

The main settings file is `ApexRadiance.toml`.

**Upgrading from an older combined build?** Remove the old combined ASI or `S3SSApex.asi` from `Game\Bin`. Keep the official standalone `Sims3SettingsSetter.asi` if you use it. Apex imports recognised legacy settings on its first start.

## Compatibility

- **Sims3SettingsSetter:** can run alongside Apex. Its active Split-Level Lighting Fix is detected so Apex does not apply the equivalent patch twice.
- **Sims 3 Performance Patch:** can run alongside Apex; the mods target different game operations.
- **DXVK:** supported, although graphics-backend behaviour can differ. Report which backend you use when sending a visual bug.

## Reporting a problem

Open **Report a problem** in the Apex menu. Save a report, record a short reproduction, capture lighting at a surface or save a lighting snapshot. Captures can have an optional title and description.

Reports are stored in dated folders under:

```text
Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\
```

Zip the relevant folder and attach it to a [GitHub issue](https://github.com/loinyx/Sims3-ApexRadiance/issues) or the mod's Bugs tab on Nexus Mods. Include what happened, how to reproduce it, your game version and whether you use DXVK or other graphics mods.

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

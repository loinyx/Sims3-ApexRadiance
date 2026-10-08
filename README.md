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

## Water and reflections

Bring the shoreline into the picture. Trees and buildings reflect in supported ponds, lakes and world water, while nearby lamps add glow after dark. Adjust the look from **Water & Snow**.

## Shape the image

Lighting is the centre of Apex Radiance; the image tools let you build a look around it.

| Tool | What you can adjust |
|---|---|
| **Color** | Brightness, contrast, saturation, temperature, tones, a six-colour mixer, clarity, sharpness and vignette |
| **25 stackable filters** | Film looks, colour moods, glow, camera effects, retro screens, LUTs and colour-blind adjustments; each has its own switch and strength |
| **Ambient Occlusion** | Contact shade around objects and buildings, with five quality levels, temporal smoothing and thin-object detail |
| **Sim Occlusion** | Separate AO strengths for Sims and hair, with a maximum-darkening limit and compatible transparent-hair support |
| **Depth Blur** | Background blur with focus controls; fades out in map view |
| **Edge Smoothing** | SMAA or FXAA for cleaner edges, with optional sharpening |
| **Banding Fix** | Fine grain and optional gradient smoothing to reduce visible colour steps |

**Color has its own Overview.** Basic, Tones, Color, Detail and Filters can be switched independently. The main Image switch disables all colour groups while preserving your adjustments. Compare your look with the game's image using the comparison controls.

**Your filters, your shortcuts.** Right-click a filter to assign an optional on/off key combination. None are assigned by default. A small tag appears to the left of the expand arrow; open the row to adjust the filter. PNG LUT files can be placed in the mod's `LUTs` folder and selected from the LUT filter.

Choose the effects that suit your game. Each has its own controls, so you can balance your preferred look with performance.

For the best compatibility with Apex effects, turn off the game's own **Edge Smoothing** in **Options > Graphics** and use Apex's version instead. The menu helps you identify settings that need attention.

## Screenshots and the in-game menu

Open the menu with **Ctrl+Shift+F11** after loading a world. Search for a setting, save a profile, choose which parts of a profile to apply, undo a change or restore an individual control to its default. Hold **Alt** over the menu to temporarily hide it. Click the version at the bottom for **What's new**.

Press **F8** to save a screenshot with Apex effects. You can hide the game's interface for the photo and choose the screenshot destination in Settings. **F10** hides the game interface during play.

## Targeted performance improvements

Move around your neighbourhood, load lots and customise Sims with options designed to reduce small, repeated pauses. Apex's **15 performance controls** target lighting updates, loading, textures, Create a Sim and other everyday game work. You can enable or disable each one from the menu.

These changes preserve the finished scene's detail. Results depend on your world, hardware and other mods; heavier visual effects still have their own performance cost.

**Lot Streaming** lets you keep more lots in full detail farther from the camera. It is experimental and off by default. Experimental memory and script options are also labelled in the menu.

[Explore the performance options →](docs/features/performance/README.md)

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

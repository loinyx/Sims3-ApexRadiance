<p align="center"><img src="docs/images/logo-256.png" width="140" alt="Apex Radiance logo"></p>

# Apex Radiance for The Sims 3

A lighting, visuals and performance mod for The Sims 3. At night, street lamps and lot lamps really light the world around them: the ground, lots, objects, fences, walls, roofs, ponds and snow. It also goes after the game's small, frequent stutters, especially while you move the camera and while lots, Sims and textures load, without changing how the game looks. On top of that come a full color editor with 25 stackable filters, soft ambient occlusion with separate control for Sims, clean anti-aliasing, a soft depth blur and filtered screenshots, all from one in-game menu.

**Supported menu languages (21):** English, Portuguese, Spanish, French, German, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Czech, Polish, Hungarian, Greek, Russian, Japanese, Korean, Thai, Simplified Chinese and Traditional Chinese. Choose a language in Settings; automatic selection follows Windows.


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

### Night Lights

In the base game, lamps glow but barely light anything around them. Apex Radiance rebuilds night lighting so they really light their surroundings:

- **Ground, lots and streets:** smooth, warm pools of light on grass, sidewalks, roads and lot floors, with no hard edge where a lot ends.
- **Objects, fences, walls and roofs:** outdoor objects, fences, stairs, house walls and roofs pick up the light of nearby lamps, and walls stop it: a yard behind a wall stays dark.
- **Every-Story Ground Light:** lamps on upper floors, balconies and terraces light the ground below too.
- **Inside the house:** indoor lamps shine through stairwells, atriums and open floors to the stories above and below, and stop at solid floors and walls. Furniture, stairs and curtains take the room's light smoothly, and walls meet at the floor line without a step in the light.
- **Rooms at Night:** instead of the game's strong blue glow in rooms with every lamp off, a soft background light you set with Brightness and Blue tint.
- **Light styles and brightness:** Subtle, Soft or Natural sets lamp brightness everywhere at once, and the ground, roads, street lamps, lot lamps, objects, fences, walls, roofs and ponds each have their own slider.
- **Lamp colors and moonlight:** from the game's pink to warm white, with an optional separate color for lot lamps, and a slider for how much the moon lights the world.
- **Lamp switches all at once (Experimental):** switching one lamp or all the lights of a house changes the rooms, the furniture, the ground and the trees together, in a single frame, once the new light is ready. No steps and no corrections afterwards.
- **Light detail (Experimental):** an option for sharper lamp light on walls and floors.
- **Fast, correct updates:** placing, moving, switching or recoloring a lamp, adding a story or a roof, and the change from day to night all update the light quickly. **Refresh lighting** relights rooms, lots and objects at any time.
### Water & Snow

- Lamps glow and sparkle on ponds and lakes.
- Ponds mirror the trees and houses along their shore, and so does the water of Twinbrook, Bridgeport and Moonlight Falls that never showed the game's reflection.
- Walked-on sidewalks show through the snow.

### Ambient Occlusion

Soft shade where things meet: under furniture, in corners, where walls meet the floor and around houses and trees. Lamp-lit and bright surfaces keep their light and color. AO always runs at full resolution; the former Half resolution option is removed, so players who used it may see a higher GPU cost. Temporal smoothing keeps it clean and steady, and Thin object detail lets the shade pass behind legs and rails. Five quality levels, adjustable strength, reach and distance, and it also works in map view.

### Sim Occlusion

Contact shade that suits furniture can look heavy on faces and hair. Sim Occlusion lets Sims receive less of it than the rest of the scene, with separate strengths for skin and clothing and for hair, a limit on how dark a Sim can get, and support for transparent hair. A preview shows exactly which pixels each control affects.

### Color

A full picture editor for the 3D world; menus and text keep their normal look.

- **Overview and group switches:** Basic, Tones, Color, Detail and Filters can be enabled separately. The main Image switch disables every color group without discarding your adjustments.
- Brightness, contrast, saturation, warmth and sharpness.
- Film-style tones, a six-color mixer and a vignette.
- A before/after switch and a hold-to-compare key.
- **Filters:** 25 looks you can stack, each with its own strength: Technicolor 1 and 2, DPX Cineon, Vintage, Cross-process, Filmic pass, Black and white, Tint, Colorfulness, Night Mode, Levels, LUT, Auto exposure, Adaptive sharpening, Glow, Halation, Dreamy, Fake HDR, Emphasize, Tilt-shift, Prism, Film grain, 3DFX, CRT and Color-blind mode. All start off.
- **Optional filter shortcuts:** right-click a filter to assign or remove your own key combination for on/off. No filter shortcut is assigned by default; assigned keys appear beside its switch. Filters are grouped in compact rows with expandable adjustments.
- **LUT files:** put PNG look-up tables (Lightroom, Photoshop or ReShade LUT packs) in the LUTs folder of Apex Radiance and pick one in the LUT filter.

### Banding Fix

An invisible, fixed grain that removes the visible steps in smooth light and shadow, on its own page, with optional smoother gradients in the sky.

### Depth Blur

Softly blurs the distant background, like a camera focused on what's near. The focus follows what you're looking at, and the effect turns itself off in map view.

### Edge Smoothing

SMAA or FXAA on the 3D world while menus stay sharp, from Low to Extreme, with edges found from the scene depth too and optional texture sharpening.

### Screenshots

Press **F8** to save a screenshot with every Apex Radiance effect applied, with the game's interface hidden for the photo if you like. Choose whether it goes to the game's Screenshots folder or to the Apex Radiance folder.

### Performance

Apex Radiance goes after the game's small, frequent stutters, especially while you move the camera and while lots, Sims and textures load, without changing how the game looks. Every option has its own switch under System › Performance, so any that misbehaves can be turned off.

- **Camera and lighting:** rooms light up sooner when you enter a lot or change floors; lot lighting and wall shading are spread out or postponed while the camera moves; new objects join the scene over a few frames instead of all at once.
- **Files and objects:** the game remembers where its files are instead of searching every package each time, skips repeated searches for files that do not exist, builds file lists faster and finds objects faster.
- **Textures and Sims:** faster texture and cache compression with exactly the same output, split over several processor cores, and faster sorting of hair and clothing layers when Sims are built.
- **Memory:** less waiting when the game reserves and frees memory, and **Room to save** (Experimental) keeps memory free for saving (Error 12) and drops unused game files when memory runs low.
- **Game and scripts (Experimental):** the game stops repainting its own window every frame, and its scripts compare numbers and look up types with less work.

Apex Radiance's own shaders are compiled once on a background thread and kept on disk, never in the middle of play.

### Lot Streaming

Lots keep their full detail farther away (up to 300 m) and more of them at once (up to 16), streaming in smoothly while the camera moves, with steadier lot visibility and a pause in map view. Experimental and off by default: turn it on in its own page under System. Research and code by [idavidveiga](https://github.com/idavidveiga).

### The menu

Open it with **Ctrl+Shift+F11** (a note shows the key when the game starts).

- **Welcome:** a new installation starts with three ready-made profiles, Performance, Default and Quality. Click one to see it in the game right away, then keep it or keep your own settings.
- **Attention:** appears only when something outside Apex Radiance blocks an effect, such as the game's own Edge Smoothing or a room color saved by Sims3SettingsSetter, and explains how to fix it.
- **Profiles:** save your setup, choose which parts go into each profile and which parts to apply when you load one.
- **Shortcuts:** pick a set (letters, numbers or F keys) and change any shortcut.
- **Hold Alt** over the menu to hide it and look at the game.
- **What's new:** click the version at the bottom of the menu to see what changed.
- Search, Undo after any change, a reset for each setting, adjustable text size, and 21 languages (every language The Sims 3 ships in), detected automatically from your Windows language.

## Requirements

- The Sims 3, Steam version 1.67.2 (`TS3W.exe`) or EA app version 1.69 (`TS3.exe`). On other versions, features whose game code is not found are shown as unavailable.
- An ASI loader in `Game\Bin`, for example [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

## Install

1. Close the game and the launcher.
2. Copy `ApexRadiance.asi` into `The Sims 3\Game\Bin\`.
3. Start the game and press **Ctrl+Shift+F11** to open the menu.

Settings are saved in `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance.toml`.

If you used the older combined build (Sims3SettingsSetter with Apex inside) or `S3SSApex.asi`, delete it from `Game\Bin` and keep the official `Sims3SettingsSetter.asi`. Apex Radiance copies your old settings on its first start.

## Reporting a problem

Open the menu's **Report a problem** page. It saves what helps fix a bug in its own dated folder under `Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\`:

- **Save a report:** the log and your settings, including the crash report after a crash.
- **Record a few seconds:** what the lighting does while you make the problem happen.
- **Capture the light at a spot:** what paints that spot, with its textures.
- **Lighting snapshot:** every lamp and room of the loaded lots.

Zip the folder and attach it to a post in the Bugs tab on Nexus Mods or a GitHub issue.

## Compatibility

- **Sims3SettingsSetter:** works alongside Apex Radiance. When its Split-Level Lighting Fix is active, Apex Radiance does not apply that patch twice.
- **Sims 3 Performance Patch:** works alongside it. The two mods change different parts of the game.
- Works with DXVK.

## Building

Visual Studio 2022 Build Tools (v143), C++20, Release | x86, static CRT, with vcpkg (`x86-windows-static`) providing Dear ImGui (dx9 + win32), Microsoft Detours and toml++.

```
MSBuild ApexRadiance.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false   -> Release\ApexRadiance.asi
```

How every feature works, including the reverse-engineered engine details, is documented in [`docs/`](docs/README.md).

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

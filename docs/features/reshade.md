# ReShade effects

Apex Radiance runs the effects of an installed ReShade on the game's picture before the game draws its interface, so
menus, plumbobs, speech bubbles and text keep their colours while the world gets the ReShade look. Depth-based effects
(depth of field, ambient occlusion, fog) read the game's exact depth from Apex Radiance. The Color > ReShade tab switches
and adjusts the effects of the current ReShade preset, picks the preset, and adds effect files, presets or whole
shader packs without leaving the game.

## Status

| | |
|---|---|
| Availability | In development (this branch); Experimental badge |
| Default | On (does nothing until ReShade 6.3 or newer is installed) |
| Menu | Color > ReShade (card ReShade and one panel per effect file); Overview > Image and Color > Overview rows |
| Configuration | `[patches.ReShade]` in `ApexRadiance.toml`; the effects, their values and the preset stay in ReShade's own files |
| Source | [`features/reshade_bridge.cpp`](../../features/reshade_bridge.cpp), [`patches/reshade_patch.cpp`](../../patches/reshade_patch.cpp) |

## The problem

ReShade applies its effects when the game presents the frame, after the game has drawn its interface. Colour grading,
sharpening, bloom and depth of field therefore also change the game's menus and text, and a depth of field blurs the
interface. ReShade's generic depth detection must guess which of the game's depth buffers holds the scene. Shader packs
are installed by copying files next to the game and editing `ReShade.ini`.

## How Apex Radiance solves it

Apex Radiance registers itself with ReShade as an add-on and renders ReShade's effects at its own post-scene boundary,
the point where the game moves from the finished 3D scene to its interface (see
[architecture](../architecture.md), post-scene chain). ReShade then skips its own pass at Present.

1. While the feature is on and ReShade is not connected, the Present hook looks for a loaded module exporting
   `ReShadeRegisterAddon` every 2 s and registers with API version 14 (ReShade 6.3).
2. The `reshade_present` event hands over the Direct3D 9 effect runtime. A runtime on another API (ReShade on Vulkan
   through DXVK) is reported and left alone.
3. At the post-scene boundary, after Color (order 50), `render_effects` draws the enabled techniques into the back
   buffer. ReShade captures and restores the device state around it.
4. In `reshade_begin_effects`, registered after ReShade's own depth add-on, Apex Radiance binds its shared INTZ scene
   depth as `DEPTH` when **Accurate distance for effects** is on.
5. A frame where the boundary did not run (loading screens, a frame with no scene) calls `render_effects` with no render
   target, which marks the frame as rendered: ReShade draws nothing over the interface in that frame.
6. **Add effects** copies `.fx` / `.fxh` files, textures and `.ini` presets, or extracts a `.zip` pack (stored or
   deflate; folders below `Shaders/` and `Textures/` are kept), into `Apex Radiance\ReShade\{Shaders,Textures,Presets}`.
   The two folders are appended (with `\**`) to ReShade's `EffectSearchPaths` and `TextureSearchPaths`, then ReShade
   reloads its effects.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| ReShade (card switch) | `enabled` | bool | true | | Apex Radiance drives ReShade's rendering point; off returns ReShade to its default point |
| Accurate distance for effects | `apexDepth` | bool | true | | Binds Apex Radiance's scene depth as ReShade's `DEPTH`; keeps the shared depth running |
| ReShade preset | (ReShade's `PresetPath`) | file | ReShade's current | `.ini` presets in ReShade's folder and `Apex Radiance\ReShade\Presets` | Switches ReShade's preset |
| Effect switches and settings | (ReShade preset) | | | from the shader's `ui_*` annotations | Saved by ReShade in the current preset |

Presets of Apex Radiance carry the feature's switch and **Accurate distance for effects** in the Color part; the
effects themselves belong to the ReShade preset.

## Compatibility and interactions

- Needs ReShade 6.3 or newer installed for Direct3D 9 (`d3d9.dll` next to `TS3W.exe`). Both the normal and the add-on
  build of ReShade accept the registration; the events used are allowed for external add-ons in the normal build.
- With DXVK, ReShade normally runs on Vulkan: the tab says so and ReShade keeps its default point.
- The game's own Edge Smoothing makes the back buffer multisampled. ReShade cannot render into it outside Present, so
  its effects stay at the default point (over the interface) and the tab shows a note.
- **Accurate distance for effects** keeps the shared INTZ depth swap running, as Depth Blur and Ambient Occlusion do.
- Turning the feature off unbinds the depth and asks ReShade to reload its effects, so its own depth add-on binds its
  choice again.

## Limitations

- Effects added through the tab are read from `Apex Radiance\ReShade`; the first addition writes the two search paths
  into ReShade's configuration.
- ZIP64 and encrypted packs are refused. Add-ons (`.addon`), DLLs and other executables inside a pack are skipped.
- Arrays and matrix settings of an effect are only adjustable in ReShade's own menu, as is the order of the effects.
- *Unverified in game:* the rendering point, the depth binding, the frame skip, preset switching and pack loading
  (see [validation](../validation/reshade.md)).

## Technical reference

- Registration uses the exported `ReShade*` functions directly; only the type headers are vendored
  ([third_party/reshade](../../third_party/reshade/README.md)). The `effect_runtime` method order and the
  `addon_event` values used are identical in the 6.0, 6.3, 6.6 and current headers.
- The PostScene effect is registered with `needsDepth = false`; its GPU cost is measured with timestamp queries around
  `render_effects`.
- Packs are inflated with `puff` (zlib contrib, [third_party/puff](../../third_party/puff/puff.h)); entry names are
  checked for absolute paths, drive letters and `..` before writing.

## See also

- [Validation](../validation/reshade.md)
- [Picture filters](picture-filters.md), [Presets](presets.md)

# FX effects

Apex Radiance runs shader effects from `.fx` files on the game's picture by itself, with nothing else installed. The
files use the ReShade FX format, so existing shader packs work as they are. Player-facing text names the file format
(FX, `.fx`) rather than ReShade; ReShade is named only in Settings > Credits. The effects draw on the finished scene
before the game draws its interface, so menus, plumbobs, speech bubbles and text keep their colours while the world gets
the effect's look. Depth-based effects (depth of field, ambient occlusion, fog) read the game's exact depth from Apex
Radiance. The Color > FX tab adds effect files, presets or whole shader packs, switches and adjusts each effect, and
picks the preset.

## Status

| | |
|---|---|
| Availability | In development (this branch); Experimental badge |
| Default | On (does nothing until effect files are added) |
| Menu | Color > FX (card FX effects and one panel per effect file); Overview > Image and Color > Overview rows |
| Configuration | `[patches.ReShade]` in `ApexRadiance.toml` (switch and preset name); the effects' values live in the preset `.ini` |
| Source | [`features/reshade_fx.cpp`](../../features/reshade_fx.cpp) (runtime), [`features/reshade_fx_compile.cpp`](../../features/reshade_fx_compile.cpp) (compiler front end), [`features/reshade_fx_files.cpp`](../../features/reshade_fx_files.cpp) (files, packs, images, cache), [`patches/reshade_patch.cpp`](../../patches/reshade_patch.cpp) (menu) |

## The problem

ReShade's effects library (SweetFX, qUINT, prod80, AstrayFX and many others) is written in ReShade FX, a shading
language that only ReShade itself can run. Installing ReShade puts a proxy `d3d9.dll` next to the game, and ReShade
applies its effects when the game presents the frame, after the interface is drawn: colour grading, sharpening, bloom
and depth of field also change the game's menus and text, and its generic depth detection has to guess which depth
buffer holds the scene.

## How Apex Radiance solves it

Apex Radiance embeds ReShade's open-source FX compiler and implements the Direct3D 9 side of ReShade's effect runtime.

1. **Files.** Effects live in `Apex Radiance\FX\Shaders`, textures in `Textures`, presets in `Presets` (all below
   the Apex Radiance documents folder). **Add effects** copies `.fx` / `.fxh` files, images and `.ini` presets, or
   extracts a `.zip` pack (stored or deflate; folders below `Shaders/` and `Textures/` are kept). Apex Radiance writes
   its own `ReShade.fxh` and a `ReShadeUI.fxh` replacement to `FX\Internal`, searched last, so packs that rely on
   them load and a pack's own copies win.
2. **Compile.** A worker thread preprocesses and parses every `.fx` (ReShade's predefined macros: `__RESHADE__` 60600,
   `__RENDERER__` 0x9000, `BUFFER_WIDTH` / `BUFFER_HEIGHT` of the back buffer, the preset's
   `PreprocessorDefinitions`, plus `RESHADE_DEPTH_INPUT_IS_REVERSED=0` because the game's depth is not reversed). The
   compiler generates shader model 3 HLSL; only effects that are on are compiled with `D3DCompile` (`vs_3_0` /
   `ps_3_0`), and the bytecode is cached in `FX\Cache` by a hash of the code.
3. **Objects.** The render thread creates the textures (render targets, or images decoded with Windows Imaging
   Component and scaled to the declared size), shaders and a shared depth-stencil. Textures with the same name in
   several effects are shared, as in ReShade.
4. **Render.** At the post-scene boundary, after Color (order 50), the enabled techniques run in the preset's order.
   `COLOR` is a copy of the back buffer, refreshed after any pass that wrote to it; `DEPTH` is Apex Radiance's INTZ scene
   depth, requested only while an enabled effect reads it. Each pass sets its render targets, blend, stencil and
   sampler states, draws a full-screen triangle list, and the device state is restored afterwards (state block plus
   render targets and depth-stencil).
5. **Uniforms.** Values from the menu and the preset go to constant registers; the special sources `frametime`,
   `framecount`, `timer`, `date`, `random`, `pingpong`, `key`, `mousepoint` and `overlay_open` are filled every frame.
   Technique annotations `enabled`, `timeout`, `toggle` and `hidden` behave as in ReShade.
6. **Presets.** The current preset is a ReShade `.ini` (`Techniques`, `TechniqueSorting`, `PreprocessorDefinitions`,
   one section per effect file), so presets move between Apex Radiance and ReShade in both directions.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| FX effects (card switch) | `enabled` | bool | true | | Runs the effects; off releases every effect object |
| Effects preset | `preset` | string | `Default.ini` | `.ini` files in `Apex Radiance\FX\Presets` | The preset whose effects and values are used; created when first saved |
| Effect switches and settings | (preset `.ini`) | | | from the shader's `ui_*` annotations | Saved in the current preset when an edit is committed |

Presets of Apex Radiance carry the feature's switch and the preset name in the Color part; the effects' values belong to
the preset `.ini`.

## Compatibility and interactions

- Needs nothing else installed. An installed ReShade keeps working on its own; running both applies the effects twice.
- Uses plain Direct3D 9 calls, so it also runs under DXVK (*unverified*).
- With the game's Edge Smoothing on, the multisampled back buffer is resolved with `StretchRect` into `COLOR`
  (*unverified*).
- While an enabled effect reads `DEPTH`, the shared INTZ depth swap runs, as for Depth Blur and Ambient Occlusion.

## Limitations

- Direct3D 9 limits apply as in ReShade on Direct3D 9: no compute shaders, no 3D textures, at most 4 render targets per
  pass and 224 constant registers. An effect beyond them shows its error in its panel and can be removed there.
- Settings that are arrays or matrices are not shown in the menu (their preset values are still read).
- The `mousedelta`, `mousebutton` and `mousewheel` sources read as zero.
- ZIP64 and encrypted packs are refused. Add-ons (`.addon`), DLLs and other executables inside a pack are skipped.
- `ReShade.fxh` uses its default far plane (1000) to linearise depth.
- *Unverified in game:* everything on this page beyond compilation (see [validation](../validation/reshade.md)).

## Technical reference

- Compiler: [third_party/reshadefx](../../third_party/reshadefx/LICENSE.md), ReShade FX by Patrick Mours (BSD-3-Clause),
  from crosire/reshade at commit 7bf9de8, built as C++17 (its `u8string` use predates C++20). `ReShade.fxh` comes from
  crosire/reshade-shaders (CC0) and is embedded as
  [reshade_fx_reshade_fxh.inc](../../features/reshade_fx_reshade_fxh.inc).
- Shader model 3 layout: each uniform takes float registers from `c(offset / 16)`; booleans and integers are stored as
  floats. The vertex shader's `c255` holds the half-texel offset, and each semantic texture's pixel size sits in the
  register the code generator names. The vertex id comes from a `FLOAT1 TEXCOORD0` stream.
- The PostScene effect is registered with `needsDepth = false`; `DEPTH` is bound only when `PostScene::SceneDepthValid()`.
  Its GPU cost is measured with timestamp queries around the run.
- Packs are inflated with `puff` (zlib contrib, [third_party/puff](../../third_party/puff/puff.h)); entry names are
  checked for absolute paths, drive letters and `..` before writing.
- Offline check: [tools/reshade_fx_test](../../tools/reshade_fx_test/fx_check.cpp) parses every `.fx` below the given
  folders and, on Windows, compiles them to shader model 3.

## Rejected approaches

- Driving an installed ReShade as an add-on (render point moved before the interface, Apex depth bound as `DEPTH`): it
  required players to install ReShade. See [history](../history/reshade.md).

## See also

- [Validation](../validation/reshade.md), [History](../history/reshade.md)
- [Picture filters](picture-filters.md), [Presets](presets.md)

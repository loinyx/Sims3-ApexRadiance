# Looks

Gives the game a ReShade look: Apex runs a ReShade or GShade preset (.ini) on the game's picture with the same shader
maths, without ReShade or GShade installed. The player picks a look, sets its strength and whether menus get it too,
and can switch, reorder and tune the preset's shaders, then save the result as a look of their own.

## Status

| | |
|---|---|
| Availability | In development (3.3.0), Experimental |
| Default | Off |
| Menu | Image > Color, tab *Shaders* (Lucide `swatch-book`): the *ReShade look* card, then the *Shaders in this look* card |
| Configuration | `[patches.Looks]` in `ApexRadiance.toml` (an older `[patches.GShadeLooks]` is read once and written under the new name) |
| Folders | `Documents\Electronic Arts\The Sims 3\Apex Radiance\Looks\` (the player's looks; created on first use); `Game\Bin\1. Shader Presets\` (GShade's, read-only, listed only when it exists) |
| Presets | Part of the Color part; the look's .ini travels in an exported ZIP |
| Shortcut | *Looks on or off*, `[ui] looks_key` (Ctrl+Shift+L) |
| Source | [`patches/looks_patch.cpp`](../../patches/looks_patch.cpp) (feature, menu), [`features/looks_preset.cpp`](../../features/looks_preset.cpp) (.ini files, sources), [`features/looks_runner.cpp`](../../features/looks_runner.cpp) (passes), [`features/looks_shaders.h`](../../features/looks_shaders.h), [`shaders/looks_common.hlsl`](../../shaders/looks_common.hlsl), [`third_party/reshade_shaders/`](../../third_party/reshade_shaders/LICENSES.md), [`tools/looks_shaders_gen.py`](../../tools/looks_shaders_gen.py) |
| Validation | [validation/looks.md](../validation/looks.md) |

## The problem

Many players dress The Sims 3 with ReShade or GShade presets. Those injectors grade the finished frame, so the game's
menus and text get the look too, they cost a second Direct3D hook chain, and they do not see Apex's own picture steps.
A preset is a plain .ini that names techniques of .fx effect files and their values; the effects themselves are
written for ReShade's runtime, not for shader model 3.0 as the game uses it.

## How Apex Radiance solves it

The shaders a look uses are ported once to ps_3_0 with their maths unchanged (`third_party/reshade_shaders`, see
[Shader sources and licences](#shader-sources-and-licences)); a generated header embeds them in the ASI and they are compiled by Apex's shader cache the first time the feature is
turned on. At run time Apex reads the chosen preset, runs its enabled techniques in the preset's `TechniqueSorting`
order with its values, and blends the result over the picture by *Strength*.

1. The look is a reference to a source and a relative path: `looks:<file>` (Apex Radiance\Looks\), `gshade:<file>`
   (GShade's folder, recursive) or `apex:<name>` (a built-in Apex look, embedded .ini text, read-only; see
   [Built-in looks](#built-in-looks)). Apex never
   stores an absolute path and never writes in GShade's folder.
2. The run list is the preset's enabled techniques, plus shaders added here (at the end), in the player's own order
   when one was set. Techniques Apex has no port for are listed as *can't run here*.
3. Each built-in shader's values are the .ini's (or the .fx default when the .ini has none) unless edited here.
4. The passes run in Apex's post-scene chain: after Apex's Color (order 45) or before it (35). With *World and menus*
   they run at the end of the frame, like GShade, over menus and loading screens; with *World only* they run on the
   first back-buffer draw after the game's bloom, before the interface.
5. Holding the card's eye, the picture-compare key (B) or the menu's eye in the Color header skips the look for as
   long as it is held; *Compare with the game* turns it off with the other Apex effects.

## Menu

**ReShade look** card: header with the status ("<look> · <n> shaders", or "Off"), the GPU cost chip, a hold-to-compare
eye ("Hold to see the game without the look") and the switch. Rows: *Look*, a dropdown grouped as Apex looks, My looks
and GShade (a search field above the list once it has more than 10 looks; *Import a preset…* last) with the Looks
folder button beside it; *Strength*; *Applies to* (World and menus / World only); under Advanced, *Order* (After Apex
Color / Before Apex Color). In developer mode a *Live counters* disclosure shows the status, the last frame's run,
skipped shaders, Reload and the GPU memory. While a LUT is also on (Color > LUTs), a note under the card says both
change the color, with *Turn off the LUT* (undoable).

**Shaders** card: "<on> of <total> on" and an ellipsis menu (*Save as a new look…*, *Put every value back*, *Back to the
look's list*). One row per built-in shader of the run list: a chevron, the name (changed dot while its values differ
from the .ini) over one short description, the switch. The name side opens its sliders (one per parameter component,
the .ini's value as each slider's default, so the changed dot and Reset work) in a rounded panel set into the card;
the panel's footer, under a line, has the bordered buttons Run earlier / Run later, "<pos> of <n>" and *Put back the
look's values* (under them when narrow). Shaders the look names but Apex cannot run are one line, "<n> can't run here",
that opens to their names. *Add a shader* opens a list of every built-in shader with a search field and a Kind filter
(Color, Sharpen, Light, Film & camera, Stylized).

*Import a preset…* opens the Windows file picker (off the render thread) and copies the .ini into Looks\: a different
file with the same name gets a numbered name ("Name-1.ini"), the same file is reused; a file without `Techniques=` or
`TechniqueSorting=` is refused. *Save as a new look…* writes Looks\<name>.ini (replacing a look of that name after a
note) with the run list, the switches and the edited values, then selects it.

## Built-in looks

Five looks ship inside the ASI (`kBuiltInLooks` in `features/looks_preset.cpp`) and are listed first, under *Apex
looks*, with translated names. They use only the ported shaders, with moderate values for a life-simulation game. They
are read-only: edits are kept as tweaks like any look's, and *Save as a new look…* copies one into the Looks folder.

| Look | Shaders in run order | Key values |
|---|---|---|
| Natural | Levels, Vibrance, LumaSharpen | Black point 4, white point 250; Vibrance 0.12; sharpening 0.40, limit 0.025 |
| Warm evening | LiftGammaGain, Tonemap, Vignette | Gain 1.04 / 1.01 / 0.95; Tonemap saturation 0.03, defog 0.015 of blue; vignette amount -0.5 |
| Cinematic | Curves, Technicolor2, Tonemap, Vignette | Luma contrast 0.15 (Catmull-Rom); Technicolor2 strength 0.25; Tonemap saturation -0.10; vignette amount -0.7 |
| Soft vintage | HDR (FakeHDR), Curves, Tint (Sepia), Vignette | HDR power 1.1, radii 0.793 / 0.85; contrast 0.06; tint strength 0.15; vignette amount -0.6 |
| Vivid | Vibrance, Colourfulness, CAS | Vibrance 0.20; colourfulness 0.30; CAS sharpening 0.5 |

`tools/looks_test` parses each one with the real loader and checks every technique and value against the shader table.

## Shader sources and licences

Each port is the author's `.fx` from the author's public repository, with only mechanical ps_3_0 changes (uniforms in
constant registers, includes and techniques replaced, ReShade's sampler names mapped to Apex's). The table of files,
repositories, authors and licences, and the licence texts, are in
[third_party/reshade_shaders/LICENSES.md](../../third_party/reshade_shaders/LICENSES.md); Settings > About shows the
texts under *Third-party licenses*.

| Source | Shaders | Licence |
|---|---|---|
| [CeeJayDK/SweetFX](https://github.com/CeeJayDK/SweetFX) | CAS (AMD's notice), Curves, DPX, FakeHDR, Levels, LiftGammaGain, LumaSharpen, Sepia (Tint), Technicolor, Technicolor2, Tonemap, Vibrance, Vignette | MIT (CAS: AMD's MIT notice in the file) |
| [crosire/reshade-shaders](https://github.com/crosire/reshade-shaders/tree/legacy), `legacy` | AdaptiveSharpen, Colourfulness (bacondither) | BSD 2-Clause style, in the file |
| [prod80/prod80-ReShade-Repository](https://github.com/prod80/prod80-ReShade-Repository) | PD80 04 Selective Color | MIT |
| [Daodan317081/reshade-shaders](https://github.com/Daodan317081/reshade-shaders) | RetroTint | BSD 3-Clause |

The earlier ports came from GShade's copies, which differ from the authors' code; the authors' versions replace them:

- GShade's dithering branches (`GSHADE_DITHER`, `TriDither`) are gone from every port and from `looks_common.hlsl`.
- FakeHDR uses the author's names again: technique `HDR`, uniforms `HDRPower`, `radius1`, `radius2`. GShade renamed
  them `FakeHDR`, `fHDRPower`, `fradius1`, `fradius2`; a preset with GShade's names is read through those names
  (`FindShader`, `PresetValues`), checked by `tools/looks_test`.
- Colourfulness: `wpmean` squares again (GShade multiplied by 2), the soft-limit floor is `1e-7` again, and
  `col_noise` defaults to on as in the `.fx`.
- Curves: the split formula picks per channel again (GShade switched all three channels when any one was below 0.5).
- LumaSharpen: GShade's `inversedAttribution` option is gone; Vibrance: GShade's `Vibrance_Luma` uniform is gone
  (commented out in the `.fx`).
- AdaptiveSharpen, CAS, Levels, Tonemap and the other GShade "light optimisations" (reordered or folded
  expressions, `const`, overloaded helpers) are back to the authors' code.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| ReShade look (card switch) | `enabled` | bool | off | | Turns the look on |
| Look | `preset` | string | empty (none chosen) | `looks:`, `gshade:` or `apex:` reference | The look that runs. An older absolute `presetPath` becomes `gshade:` under GShade's folder, `looks:` under Looks\, else `looks:<file name>`; an older empty one, `gshade:[Amelie] Serene 2.0.ini` |
| Strength | `mix` | float | 100% | 0 to 100% | How much of the look is blended over the game |
| Applies to | `onUi` | bool | off (World only) | | On: World and menus, the interface, menus and loading screens get the look too |
| Advanced > Order | `beforeColor` | bool | off (After Apex Color) | | Runs the look before Apex's Color instead of after it |
| Shader switches | `off` | array of technique names | empty | at most 256 read | The look's shaders switched off here |
| Add a shader | `extra` | array of `Tech@File.fx` | empty | at most 256 read | Built-in shaders added to the look |
| Run earlier / Run later | `runOrder` | array of `Tech@File.fx` | empty (the .ini's order) | at most 256 read | The run order |
| Shader sliders | `tweaks` | table: technique = 4 floats per parameter | empty | at most 64 shaders, 256 values each | Edited values; a shader back at the .ini's values is dropped |

Older keys `presetPath`, `useTweaks` (a saved false drops the tweaks once) and `protectUi` are read once and no longer
written.

## Presets

`[patches.Looks]` belongs to the Color part. Exporting a preset whose Color part has a `looks:` or `gshade:` look adds
the .ini to the ZIP as `Looks/<file name>` (stored entries and checksums, as for `LUTs/`) and stores the reference as
`looks:<file name>`; the export is a ZIP whenever a LUT or a look file goes with it. Import writes the file into
Apex Radiance\Looks\: identical bytes reuse the existing file, a different file of that name gets a numbered name and
the reference follows it; a package whose file is not the one its profile names, or that has a path in the name, is
refused. Built-in looks (`apex:`) need no file.

## Compatibility and interactions

- **Color (Picture):** *Order* puts the look after or before Apex's Color. A LUT that copies the same look grades the
  picture twice; the card says so and offers to turn the LUT off.
- **Captures:** `%PresetName%` in file names and *Save the look's preset too* use the look's .ini; *Look only while a
  look is on* captures the frame before the look (`ApexCapture::BeforeLook`).
- **ReShade or GShade installed too:** both grade the picture; turn one off.
- **RTX Remix bridge:** the device state the passes change is put back by hand and by a state block, for getters the
  bridge does not answer.

## Limitations

- Only the 17 ported shaders run (Levels, Technicolor2, RetroTint, DPX, AdaptiveSharpen, Vibrance, Tint (Sepia),
  Tonemap, Colourfulness, LumaSharpen, CAS, Curves, Vignette, Technicolor, HDR (FakeHDR), PD80 Selective Color,
  LiftGammaGain); other techniques of a preset are listed as *can't run here*.
- `PreprocessorDefinitions` of a preset are not applied: the ports are compiled with the .fx defaults.
- *Reset all settings* resets the switch, Strength, Applies to and Order; the chosen look and its edits stay.

## Technical reference

- Post-scene order 45 (after Picture) or 35 (before); the end-of-frame part runs from `endSceneBeforeOverlay` on the
  back buffer; the after-bloom part waits for the game's bloom strip (one full-screen two-triangle strip) and runs at
  the next back-buffer draw.
- Each run copies the target to an A8R8G8B8 ping texture, runs the steps through ping-pong targets (AdaptiveSharpen
  adds a G16R16F target), and writes RGB back, blended by *Strength*; the last step draws straight into an 8-bit target
  at full strength.
- Constants: c0 `1/W, 1/H, W, H`, c1 pass data, c2 depth far plane; parameters from c8 (PD80 Selective Color uses up to
  c63). ReShade's uniform sources timer, frametime, framecount and random are filled once per frame.
- Shader registration is lazy (`RegisterShaders` on the first Install); the cache compiles them on a new worker run.
- Everything read from files is bounded: a preset over 1 MB is refused, at most 256 techniques are kept, at most 2000
  files are listed per source, and non-finite values are ignored.

## Rejected approaches

- **A converted-effect package (`GShadeLooks.pak`)** with every GShade .fx converted offline: large, unlicensed for
  most effects, and it tied the feature to one GShade release. Only hand-checked ports with a licence remain.
- **Reading shaders from a GShade installation** at build or run time: the build depended on a local install and the
  result on its version; the ports and their licences now live in `third_party/reshade_shaders`.
- **A two-pane menu** (run list beside a values pane): it did not fit the 400-unit minimum width or text size 200%;
  each shader's values now open under its row.
- **Clarity, HighPassSharp and FilmicPass ports:** removed, no licence in their sources.
- **Ports of GShade's copies of the shaders:** GShade edited the maths, names and notices; the ports follow the
  authors' repositories instead.

## See also

- [validation/looks.md](../validation/looks.md)
- [picture-filters.md](picture-filters.md) (Color, LUTs), [screenshots-and-video.md](screenshots-and-video.md), [presets.md](presets.md)
- [ui.md](../ui.md#presets)

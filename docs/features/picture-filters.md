# Picture filters

Picture filters adjust how the game world looks: brightness, contrast, saturation, colour temperature, tone zones,
film-style split toning, a per-colour mixer, sharpness, clarity and a vignette, plus a **Filters** tab of stackable
looks (film stocks, colour moods, glow and haze, camera effects, retro screens and a colour-blind mode), each with its
own strength. Only the 3D scene is graded; the game's menus, pie menus, tooltips, the HUD and the Apex menu keep their
own colours. Grading adds no banding of its own. A before / after split and a hold-to-compare button show the difference
at any time. In the menu the **Color > Overview** tab owns the *Picture* master switch and comparison tools. Each adjustment tab has its own switch.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (present in the first version in this repository). Filters tab and LUT files: Released in 2.7.0 |
| Default | Off; every filter off |
| Menu | Image > Color (tabs Overview, Basic, Tones, Color, Detail, Filters, LUTs); *Smooth gradients* on the Image > Banding Fix page; Overview > Image > Picture |
| Configuration | `[qol.picture]` and `[qol.picture.filters]` in `ApexRadiance.toml`; compare key `[ui] picture_compare_key`; LUT files in `Documents\Electronic Arts\The Sims 3\Apex Radiance\LUTs\` |
| Source | [`features/picture.cpp`](../../features/picture.cpp), [`features/picture.h`](../../features/picture.h), [`framework/d3d9_bootstrap.cpp`](../../framework/d3d9_bootstrap.cpp) (fire points), [`apex_gui.cpp`](../../apex_gui.cpp) (`ColorPage`, `BandingPage`) |

## The problem

The game offers no colour grading. A filter applied to the whole final frame (as ReShade does) also changes the
interface, which makes text and icons look wrong. Grading 8-bit data also turns smooth gradients into visible steps, and
the game's own DXT textures (5 to 6-bit colour endpoints) and 8-bit light maps already band.

## How Apex Radiance solves it

Apex Radiance grades the finished scene as the last effect in the shared post-scene chain, before the game draws its
interface. The grade and the filters run in linear light and are re-quantised to 8 bits with a fixed dither below one
step. The end-of-frame scene-copy mask is a fallback, not the normal colour path.

The hidden-UI boundary learns the game's scratch-target identity from an already-filtered UI-visible
frame. It recognises the actual point-filtered 256x256 first-tile rectangles into the 2048x1024 A8R8G8B8 target.
With UI hidden, that same target/layout, no UI already drawn, matching shared depth, no multisampling, no depth write,
LESSEQUAL depth test and complete RGB or RGBA write mask allow the ordered chain to run before the first tile is copied.
The visible boundary and unknown-layout EndScene fallback are unchanged; Reset drops the learned identity.
No strength, colour parameter, shader or lighting solver is changed. Visual parity still requires gameplay testing.
If a session starts with UI hidden and the target has not been learned, the old fallback remains until a visible frame.

1. **Find the end of the scene.** Picture counts depth-tested draws into the back buffer. A draw with the depth test on
   but `D3DCMP_ALWAYS` and no depth write uses no depth and counts as depth-off. At every switch from depth-tested to
   depth-off drawing (after at least 4 scene draws) it copies the back buffer; the last copy of the frame wins, except
   that after a first copy a run of fewer than 4 depth-tested draws is treated as UI.
   When the switch is the game's bloom composite (a 2-primitive triangle strip right after the scene), the copy is taken
   after the bloom. While Emphasize is on, the scene depth is copied at the same moment.
2. **End of frame.** If the frame ended on the scene (no game UI after it), the copy is taken just before the Apex
   overlay.
3. **The pass** normally runs at the post-scene boundary, before UI, only while a world is loaded. The masked
   end-of-frame path runs only if the boundary did not apply it and a scene copy is available.

**Filters.** Every filter of the Filters tab is a branch of the same pass, skipped when its switch is off, so a filter
that is off costs nothing. The filters add up: any number can be on at once, in a fixed order (see *Technical
reference*). Filters that look around the pixel (prism, sharpening, CRT, 3DFX) read the scene copy, so a button or a
panel is never pulled into the world. They apply only while the Picture card is on.

## LUTs

Color > LUTs has its own switch and a single file selector, Amount control, reset action, folder button and About LUTs
explanation. It follows the Color master switch independently of the Filters group. A missing saved filename remains
visible until the player chooses another file; Apex does not silently substitute a different look. Long filenames use
the shared full-text tooltip; the selected name is not repeated below the selector.

Supported files are PNG strips (height 8–128, width equal to height squared) and 3D CUBE tables with side length 2–65,
red channel varying fastest, and supported `DOMAIN_MIN`/`DOMAIN_MAX`. A 1D LUT or combined shaper/3D LUT is refused
with a status message. A preset export bundles the selected LUT automatically when Color is included; see
[Presets](../ui.md#presets). Folder locations and legacy TOML references stay compatible.

## Settings

All values are stored in `[qol.picture]`; the filters in its sub-table `[qol.picture.filters]`. Sliders apply live while
dragging and save when released. Missing keys keep their defaults.

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Picture (card switch) | `enabled` | bool | off | | Turns the grade and the filters on; the rows stay visible, greyed, while off |
| Before / after (header button) | (not saved) | bool | off | | Left half without Picture, right half with it, red one-pixel divider |
| Hold to compare (eye button, or the compare key over the menu) | `[ui] picture_compare_key` | key | B | | While held, the game's original picture is shown |
| Basic > Brightness | `exposure` | float | 100% (0 EV) | 25 to 400% (-2 to +2 EV) | Linear gain in stops; menus unchanged |
| Basic > Contrast | `contrast` | float | 100% | 60 to 150% | Power curve around mid grey (0.18 linear) |
| Basic > Saturation | `saturation` | float | 100% | 0 to 160% | 0% is black and white |
| Basic > Temperature | `temperature` | float | 0 | -100 to +100 | Cooler (blue) to warmer (gold) |
| Basic > Sharpness | `sharpen` | float | 0% | 0 to 150% | 4-neighbour unsharp mask, clamped to the neighbours' range (no halos) |
| Tones > Midtones | `midtones` | float | 100% | 60 to 160% | Brightens or darkens middle tones; black and white stay |
| Tones > Shadows | `shadows` | float | 0 | -100 to +100 | Up to plus or minus 50% gain below luminance 0.25 |
| Tones > Highlights | `highlights` | float | 0 | -100 to +100 | Up to plus or minus 30% gain from luminance 0.3 to 1 |
| Tones > Blacks | `blacks` | float | 0 | -100 (Faded) to +100 (Deeper) | Lifts or deepens blacks by up to 2% of white |
| Color > Tint | `tint` | float | 0 | -100 (Green) to +100 (Magenta) | Green gain `1 - 0.06 t` |
| Color > Vibrance | `vibrance` | float | 0 | -100 to +100 | Saturates dull colours more than vivid ones |
| Color > Film tones > Shadow color | `shadow_hue` | float | 215 | 0 to 360 degrees | Hue of the shadow tint; a swatch shows the colour |
| Color > Film tones > Shadow amount | `shadow_tint` | float | 0% | 0 to 100% | 0% is off |
| Color > Film tones > Highlight color | `highlight_hue` | float | 40 | 0 to 360 degrees | Hue of the highlight tint |
| Color > Film tones > Highlight amount | `highlight_tint` | float | 0% | 0 to 100% | 0% is off |
| Color > Color mixer > Reds, Yellows, Greens, Cyans, Blues, Magentas | `mixer` | array of 6 floats | 100% each | 0 to 200% | Saturation per hue family (Greens: lower for less neon plants) |
| Detail > Clarity | `clarity` | float | 0 | -100 (Softer) to +100 (Crisper) | Local midtone contrast against a 1/8-size copy of the scene |
| Detail > Vignette | `vignette` | float | 0% | 0 to 100% (stored 0 to 0.8) | Darker corners |
| Detail > Vignette size | `vignette_size` | float | 50% | 0 to 95% | Radius (0 centre, 1 corner) where the darkening starts |
| Banding Fix > Smooth gradients | `deband` | float | 8% | 0 to 200% | Deband filter; follows the Banding Fix switch, not Picture's (see [banding-fix.md](banding-fix.md)) |

*Film tones* and *Color mixer* are collapsed *Advanced* groups.

### Filters

The Filters tab groups all 25 existing filters into six compact family panels. Each row has its own switch and
adjustment disclosure; controls are available when expanded, and disabled while that filter, its group or Picture is off.
Fine tuning remains under *Advanced*. Every switch is a bool, off by default. Sliders
shown as a percentage store a fraction (100% = 1.0); signed sliders show -100 to +100 and store -1 to 1; colour sliders
store a hue in degrees (0 to 360). A filter runs when its switch is on and its Amount is not 0.

| Card | Switch key | Controls: menu label `key` default (range) | What it does |
|---|---|---|---|
| **Film looks** | | | |
| Technicolor 1 | `technicolor1` | Amount `technicolor1_amount` 60%; Cyan side `technicolor1_cyan` 0 (Greener -100 to Bluer +100); Saturation `technicolor1_saturation` 100% (0 to 200%) | Classic two-strip film: every colour becomes a red or a cyan record |
| Technicolor 2 | `technicolor2` | Amount `technicolor2_amount` 50%; Saturation `technicolor2_saturation` 100% (0 to 200%); Brightness `technicolor2_brightness` 0 (Darker to Brighter); Advanced > Dyes: Red, Green, Blue dye `technicolor2_dye` [100%, 100%, 100%] (0 to 200%) | Three-strip dye transfer: each dye subtracts its complement, dense pure primaries |
| DPX Cineon | `dpx` | Amount `dpx_amount` 50%; Contrast `dpx_contrast` 50%; Saturation `dpx_saturation` 100% (0 to 200%); Advanced > Color curves: Red, Green, Blue curve `dpx_curve` [100%, 100%, 100%] (50 to 150%) | Cinema negative: a film S curve per channel, black and white kept |
| Vintage | `vintage` | Amount `vintage_amount` 70%; Fade `vintage_fade` 50%; Warmth `vintage_warmth` +50 (Cooler to Warmer); Faded colors `vintage_colors` 40% | A faded print: lifted blacks, softer whites, warm cast, washed-out colours |
| Cross-process | `cross_process` | Amount `cross_amount` 50%; Contrast `cross_contrast` 50% | Slide film in negative chemistry: green-cyan shadows, yellow highlights, more contrast |
| Filmic pass | `filmic_pass` | Amount `filmic_amount` 85%; Fade `filmic_fade` 40%; Contrast `filmic_contrast` 100% (0 to 200%); Bleach `filmic_bleach` 0%; Saturation `filmic_saturation` -15 (Muted to Vivid); Advanced > Color curves: `filmic_curve` [100%, 100%, 100%] (50 to 150%) | A filmic grade on the encoded picture: S-curve contrast, a brightness curve per channel, bleach bypass, saturation and fade |
| Black and white | `black_and_white` | Amount `bw_amount` 100%; Filter color `bw_filter_hue` 30; Filter strength `bw_filter` 50%; Contrast `bw_contrast` 0 (Softer to Punchier); Advanced > Toning: Tone color `bw_tone_hue` 35, Tone amount `bw_tone` 0% | Luminance through a coloured lens filter (red darkens skies, green lightens leaves), contrast, then an optional toning |
| Tint | `tint_filter` | Amount `tint_filter_amount` 58%; Color `tint_filter_hue` 35 | The picture's grey in one colour (sepia by default), mixed in |
| **Color and mood** | | | |
| Colorfulness | `colourfulness` | Amount `colourfulness_amount` +40 (Muted to Vivid); Advanced: Protect bright colors `colourfulness_protect` 70% | More chroma, less for colours that are already strong or bright |
| Night Mode | `night_mode` | Amount `night_amount` 60%; Darkness `night_darkness` 44% (0 to 100%, stored 0.35 of 0 to 0.8); Blue tint `night_blue` 50%; Keep lamp light `night_keep_lamps` 60% | Darker, bluer and less colourful; lamp-lit and bright areas keep their own colour |
| Levels | `levels` | Black point `levels_black` 16 (0 to 255, stored /255); White point `levels_white` 235 (0 to 255) | The black point goes to black and the white point to white. The white point stays at least 0.02 above the black point. Runs when black > 0 or white < 255 |
| **Light and detail** | | | |
| Auto exposure | `auto_exposure` | Amount `auto_exposure_amount` 70%; Target brightness `auto_exposure_target` 50%; Speed `auto_exposure_speed` 40%; Range `auto_exposure_range` 50% | The picture slowly adapts toward a target brightness from the scene's average, like the eye |
| Adaptive sharpening | `cas` | Sharpness `cas_amount` 50% | Contrast-adaptive sharpening: strong on soft detail, none on hard edges (no halos). Runs whenever its switch is on |
| Glow | `glow` | Amount `glow_amount` 40%; Threshold `glow_threshold` 60% (0 to 95%); Size `glow_size` 50%; Advanced: Warmth `glow_warmth` 0 (Cooler to Warmer) | A soft halo from the bright parts of a blurred copy of the scene: lamps, windows, sky |
| Halation | `halation` | Amount `halation_amount` 40%; Threshold `halation_threshold` 70% (0 to 95%); Advanced: Halo color `halation_hue` 15 | Film's tight reddish halo around the strongest light |
| Dreamy | `dreamy` | Amount `dreamy_amount` 40%; Softness `dreamy_softness` 60%; Saturation `dreamy_saturation` 30% | Orton effect: the picture screened with a soft blurred copy, a little more colour |
| Fake HDR | `fake_hdr` | Amount `fake_hdr_amount` 50%; Shadows `fake_hdr_shadows` 40%; Highlights `fake_hdr_highlights` 40%; Radius `fake_hdr_radius` 50%; Advanced: Halo protection `fake_hdr_halo` 60%, Saturation `fake_hdr_saturation` 10% | Local contrast in log luminance: shadows lifted, highlights pulled down, detail boosted, held back at strong edges so no halos form |
| **Camera** | | | |
| Emphasize | `emphasize` | Amount `emphasize_amount` 80%; Focus depth `emphasize_width` 50% (0 to 200% of the focus distance); Grey amount `emphasize_grey` 50%; Advanced: Automatic focus `emphasize_auto` on, Focus distance `emphasize_distance` 30 m (1 to 300 m, shown while automatic focus is off), Edge softness `emphasize_softness` 50% (5 to 100%) | Full colour in a band of distance around the focus (the centre of the screen, or a set distance), grey outside it. Needs the scene depth |
| Tilt-shift | `tilt_shift` | Amount `tilt_amount` 70%; Position `tilt_center` 55% (0% top, 100% bottom); Sharp band `tilt_width` 25%; Advanced: Toy colors `tilt_saturation` 25% | Miniature effect: a sharp horizontal band, the picture blurring more and more above and below it |
| Prism | `prism` | Amount `prism_amount` 35%; Edge start `prism_start` 35% (0 to 95%); Advanced: Quality `prism_quality` 0.5 (Low, Medium, High) | Chromatic aberration growing toward the edges, several taps across a small spectrum |
| Film grain | `grain` | Amount `grain_amount` 30%; Grain size `grain_size` 30%; Advanced: More in the shadows `grain_shadows` 50% | Smooth value noise in two sizes multiplying the brightness, strongest in the midtones, the same pattern every frame |
| **Retro and style** | | | |
| 3DFX | `retro_3dfx` | Amount `3dfx_amount` 100%; Color depth `3dfx_color_depth` 0.5 (shown as 16, 13 or 10-bit); Scanlines `3dfx_scanlines` 30%; Advanced: Dithering `3dfx_dither` 60%, Soft pixels `3dfx_soft_pixels` 30%, Gamma `3dfx_gamma` 100% (50 to 200%) | A late-90s 3D card: fewer colour levels with a 4x4 ordered dither, scanlines, a slight horizontal blur |
| CRT | `crt` | Amount `crt_amount` 100%; Curvature `crt_curvature` 30%; Phosphor mask `crt_mask` 40%; Scanlines `crt_scanlines` 40%; Advanced: Dark edges `crt_edges` 40% | An old TV: curved glass (black outside it), aperture-grille stripes, scanlines, darker edges |
| **Accessibility** | | | |
| Color-blind mode | `daltonize` | Type `daltonize_type` 1 = Green (deuteranopia) (0 = Red (protanopia), 2 = Blue (tritanopia)); Amount `daltonize_amount` 100% | Simulates the chosen colour deficiency and moves the colours that cannot be told apart into channels that can |

Unless a range is given, a percentage slider runs from 0 to 100%. Emphasize's Focus depth was a distance in metres in
early test builds; a saved value outside 0 to 2 is reset to the default when loaded.

## Compatibility and interactions

- **Post-scene effects** (Ambient Occlusion, Edge Smoothing, Depth Blur) run at the end of the scene, before Picture's
  copy (Picture's draw hooks run at priority 10, after the post-scene trigger's `Priority::First` hooks; with the game
  UI hidden the post-scene fallback runs before `BeforeOverlay`). Their pixels are inside both the frame and the scene
  copy, so they are graded like the rest of the scene.
- **Banding Fix:** *Smooth gradients* follows the Banding Fix switch. With Picture off and the Banding Fix on, the pass
  runs with only the deband (every other control and every filter neutral), and never in a frame without a scene copy,
  so it cannot smooth the game's menus.
- **The game's Edge Smoothing (MSAA):** when the game does not draw its scene straight into the back buffer, there is no
  scene copy. The whole picture, menus included, is then graded, and after 2 s the card shows: "Color also tints the
  game's menus here ... turning off the game's own Edge Smoothing (Options > Graphics) usually fixes it". Atmospheric
  fog and Emphasize also need the shared scene depth; without it their cards show "Needs the scene depth: turn off the
  game's Edge Smoothing (Options > Graphics)" and they do nothing.
- **Pie menu:** its translucent backing box is a faint veil, filtered with the scene and applied again (see the UI mask
  below); its 3D Sim portrait is UI, and fog and Emphasize read the depth copied with the scene, not the live depth whose
  square the portrait clears.
- **Night Lighting** replaces some draws and returns `Skip`; Picture's draw hooks run before any feature that may skip a
  draw (`Normal` priority), so they see every scene draw. Re-issued draws are counted twice (harmless). The pond pass is
  marked as an internal pass and ignored.
- **Apex menu:** drawn after `BeforeOverlay`, so it is always treated as UI, including in frames with no game UI.
- **Filtered screenshots:** the screenshot shortcut reads the back buffer at Present, after the Picture pass. With the
  menu open, the bootstrap runs Picture once and then fires `filteredSceneBeforeOverlay`, before the menu draws. See
  [bug-reports.md](bug-reports.md#screenshot-settings).
- **Compare with the game** (shortcut, Ctrl+Shift+F10 with the default function-key preset) turns Picture off with the
  other effects, without saving. A configuration saved meanwhile still stores Picture as on.
- **Profiles** carry `[qol.picture]` (filters included) in their Color part. Applying a profile goes through
  `SetParams`.
- **All effects** (Overview) includes Picture.
- **Resolution changes and borderless modes:** sizes come from the back-buffer description, not the window.
- **Device reset** (alt-tab, resolution change): resources are released before the reset and recreated at the next
  pass; the LUT is loaded again.

## Limitations

- No scene copy, no separation: frames with fewer than 20 depth-tested back-buffer draws are graded as a whole. Outside
  a loaded world (main menu, loading screens) the pass is idle, and loading frames are skipped until shader
  precompilation completes.
- The first frame after turning Picture on is not graded (its hooks register in that frame).
- If the game draws a long depth-tested run (20 draws or more) after it started its UI, the next depth-off draw retakes
  the copy and the UI drawn before it counts as scene (inferred from the "last copy wins" rule; no case observed).
- The pass runs once per frame, at the first EndScene where render target 0 is the back buffer. Draws after it in the
  same frame are not graded (inferred; not observed).
- UI pixels that differ from the scene by less than 4 levels are graded as scene; between 4 and 24 levels they are
  partly graded. A faint translucent veil (under 4 levels) is re-applied as a ratio over the graded scene.
- Deband and sharpening near UI edges: the deband threshold excludes most UI pixels and sharpening clamps to the
  neighbour range.
- 8-bit output: Brightness and Highlights pushed above white clip at the encode.
- With Picture on and every slider neutral, the image still changes slightly (the fixed dither).
- Emphasize needs the shared scene depth (off while the game's Edge Smoothing is on).
- LUTs accept PNG strips and 3D CUBE files. 1D and combined shaper/3D CUBE files are refused.

## Technical reference

**Fire points** (`framework/d3d9_bootstrap.cpp`, inside the game's EndScene):

1. `RenderCallbacks::endSceneBeforeOverlay` (the post-scene fallback among others).
2. `Picture::BeforeOverlay`: takes the copy when the effective grade is on, the frame is pending, at least 20 scene draws
   occurred and the frame ended on the scene (`lastWasScene` with no copy yet or a run of at least 20 draws, or
   `copyAfterStrip`).
3. If the overlay is visible and a capture screenshot is pending: `Picture::OnEndScene` now (it consumes `frameReady`,
   so the later call does not grade twice).
4. `RenderCallbacks::filteredSceneBeforeOverlay`.
5. The Apex overlay.
6. `Picture::OnEndScene`: the pass.
7. Device reset: `Picture::BeforeReset` releases resources and forgets RT0 and the back buffer.

**Hooks** (registry name `"Picture"`, registered at the first pass while on, removed while off): Present and
SetRenderTarget at `Priority::First`; DIP and DP at priority 10 (after the post-scene trigger, before `Early` = 25 and
`Normal` = 50).

- `OnFrameBoundary`: re-reads the back buffer and, after a reset, RT0 (a reset sets RT0 without a `SetRenderTarget`
  call); resets `frameReady`, `sceneDraws`, `runDraws`, `lastWasScene`, `copyAfterStrip`, `sceneCopied`.
- `OnGameDraw`: ignores internal passes and draws not into the back buffer. Depth on (and not `ALWAYS` without depth
  write): `sceneDraws++`, `runDraws++`, `lastWasScene`. Depth off with `sceneDraws >= 20` right after a scene draw: after
  a first copy, a run under 20 draws is ignored; a `D3DPT_TRIANGLESTRIP` `DrawPrimitive` of exactly 2 primitives is the
  bloom composite (copy at the next back-buffer draw); anything else copies now (`CopyScene`:
  `StretchRect(backbuffer -> sceneSurf, D3DTEXF_NONE)`, then the depth copy when requested).

**Pass** (`Picture::OnEndScene`): skipped while precompile is incomplete, while held for compare, when RT0 is not the
back buffer, outside a loaded world (`WorldSession::InWorld`), or (Picture off, deband only) without a scene copy. Then:

1. Read previous timestamps; begin a new set (4 rotating sets, `ms = 0.9 ms + 0.1 v`).
2. `StretchRect(backbuffer -> frameSurf, D3DTEXF_NONE)`.
3. Work out the effective filters (switch on and amount above 0.001); load the LUT when its file changed.
4. If clarity, Glow, Halation, Dreamy, Tilt-shift, Fake HDR or Auto exposure is on: build the 1/2, 1/4,
   1/8 chain with linear `StretchRect`s from `sceneSurf` (or `frameSurf` without a copy).
5. Save 12 render states, 8 samplers x 6 sampler states and textures, PS, VS, declaration / FVF, stream 0, PS constants
   c0..c56 and the viewport. Neutral state (Z, blend, alpha test, stencil, scissor, fog, sRGB write, clip planes off; cull
   none; colour write 0xF).
6. Auto exposure first renders its 1x1 pass (`AdaptPS`) into the other of two targets, reading the previous value.
7. Bind the samplers, set the shader and the constants; draw one full-screen `DrawPrimitiveUP` quad (FVF
   `XYZRHW | TEX1`, -0.5 pixel offset) into the back buffer, reading the frame copy.
8. Restore everything (stream 0 explicitly) and end the timestamps.

After each settings change the next 3 passes read the constants and bound shader back and log
`[Picture] Pass check: ...` when another mod changed them on the way.

**Resources** (`InitResources`, `D3DPOOL_DEFAULT` render targets in the back buffer's format, normally
`D3DFMT_A8R8G8B8`): `frameTex`, `sceneTex` (back-buffer size), `chainTex[0..2]` (ceil halvings); `adaptTex[0..1]` (1x1
`D3DFMT_A16B16G16R16F`, optional: without them Auto exposure does nothing); `depthTex` (R32F copy of the scene depth,
created while fog or Emphasize asks for it); `lutTex` (`D3DPOOL_MANAGED` A8R8G8B8, decoded with WIC). Shaders
`PicturePS` ("picture.hlsl"), `DepthCopyPS` and `AdaptPS`, ps_3_0, `D3DCOMPILE_OPTIMIZATION_LEVEL3`, compiled at
start-up on a background thread (`framework/shader_cache.h`); creation failure is permanent until restart. Log:
`[Picture] Resources ready (WxH, format N)`, `[Picture] LUT loaded: <file> (N cells per side)`.

**Samplers:** s0 frame (point), s1 scene copy (point), s2 1/8 scene (bilinear), s3 scene depth (point, only for fog and
Emphasize), s4 1/2 scene and s5 1/4 scene (bilinear), s6 LUT strip (bilinear), s7 the adapted luminance (1x1).

**Shader** (`PicturePS`), per pixel:

1. **UI mask:** `d = max_channel(|frame - scene|)`; `ui = sceneCopied ? smoothstep(4/255, 24/255, d) : 0`. Without a
   copy the whole frame is graded. A faint veil (`d > 0.25/255`) keeps the ratio
   `veil = min((frame + 1/255) / (scene + 1/255), 4)`, applied again after the encode.
2. **Sample position and neighbour filters** (scene pixels): CRT curvature moves the sample position; then deband,
   Prism, 3DFX soft pixels, sharpening and Adaptive sharpening, each reading the scene copy (`SceneTap`); Tilt-shift
   blends toward the 1/2 and 1/4 copies.
3. **Deband** (scene pixels, `ui < 0.5`): average with samples on two rings of 8 fixed directions (ring 2 rotated
   22.5 degrees), radii `12 x H/2160` and `32 x H/2160` px, accepting only samples whose max channel difference is
   below `deband x 6/255` (gamma-encoded). Deterministic.
4. **Sharpen** (scene pixels): `fs + s x (fs - mean of 4 neighbours)`, clamped to `[min(neighbours, fs),
   max(neighbours, fs)]`.
5. **Decode** gamma 2.2: `pow(max(c, 0), 2.2)`; then **Auto exposure** (gain toward the target, clamped to its range).
6. **Clarity** (scene pixels): `r = log2(Lpixel / Lbase)`, `Lbase` from a cubic B-spline sample of the 1/8 scene (4
   bilinear taps); `g *= exp2(clarity x r / (1 + r^2) x 4e(1 - e))`, `e = saturate(Lpixel^(1/2.2))`. Strong edges get
   almost nothing. Then **Fake HDR**.
7. **Exposure and white balance:** `g *= exp2(EV) x wb.rgb`. White balance: red `1 + 0.08t`, blue `1 - 0.08t`, green
   `1 - 0.06 tint`, normalised by their Rec.709 luminance.
8. **Contrast:** `g = 0.18 x (g / 0.18)^contrast`.
9. **Tone zones** on luminance (0.2126, 0.7152, 0.0722), hue kept: `Lm = L < 1 ? L^(1/midtones) : L`;
   `Lm *= (1 + 0.5 shadows (1 - smoothstep(0, 0.25, Lm))) x (1 + 0.3 highlights smoothstep(0.3, 1, Lm))`; `g *= Lm / L`.
10. **Blacks:** `b = blacks x 0.02`; `b >= 0: max(g - b, 0) / (1 - b)`, else `g (1 + b) - b`.
11. **Split toning:** `e = saturate(L^(1/2.2))`; `g *= lerp(1, tintS, amountS (1 - smoothstep(0, 0.55, e))) x
    lerp(1, tintH, amountH smoothstep(0.45, 1, e))`. Tints come from `SplitToneColour` on the CPU: the fully saturated
    hue colour `c`, then `1 + 0.5 (c - lum(c))`, whose luminance is exactly 1.
12. **Vibrance:** `sat0 = (max - min) / max`; `g = max(lerp(L, g, 1 + vibrance (1 - sat0)), 0)`.
13. **Saturation and mixer:** `sat = saturation x (mixer on ? MixerSaturation(g) : 1)`, times Tilt-shift's toy colours
    while it is on; `g = max(lerp(L, g, sat), 0)`. `MixerSaturation`: hue 0..6 on gamma-2.2 values, six triangular bands
    of width 1 whose weights sum to 1; greys (sat < 1e-5) get 1. The mixer is on when any value differs from 1 by more
    than 0.001.
14. **Colour looks** (`ColorLooks`, in this order: Technicolor 1, Technicolor 2, DPX
    Cineon, Colorfulness, Night Mode, Vintage, Cross-process, Black and white, Filmic pass, Tint, Levels, LUT,
    Color-blind mode), then **Emphasize**, then the **light filters** (`LightFilters`: Glow, Halation, Dreamy).
15. **Vignette:** `q = (uv - 0.5) x (W/H, 1)`, `r = |q| / |(W/H, 1) x 0.5|`, `v = smoothstep(size, 1, r)`,
    `g *= 1 - amount x v^2 (3 - 2v)`.
16. **Encode** `o = pow(saturate(g), 1/2.2)`; then Film grain, 3DFX (posterise with a 4x4 Bayer dither and scanlines)
    and the CRT mask, scanlines and edges.
17. **Dither** (skipped after 3DFX): `o += (IGN(px) - 0.5) / 255`,
    `IGN = frac(52.9829189 x frac(dot(px, (0.06711056, 0.00583715))))`, the same pattern every frame. Then
    `o = saturate(o x veil)`. Compare: the left half `o = frame`, the divider red.
18. Output `lerp(o, frame, ui)`.

**Constants** (c0..c12, the grade):

| Reg | x | y | z | w |
|---|---|---|---|---|
| c0 `cLook` | saturation (0 to 2) | scene copy valid | compare | |
| c1 `cSize` | 1/W | 1/H | W | H |
| c2 `cGrade` | exp2(exposure), exposure clamped -3 to 3 | contrast (0.5 to 1.8) | blacks x 0.02 | |
| c3 `cWb` | wbR / wbL | wbG / wbL | wbB / wbL | |
| c4 `cDeband` | deband x 6/255 (deband 0 to 2) | 12 x H/2160 | 32 x H/2160 | on (deband > 0.001) |
| c5 `cTone` | 1 / midtones (0.5 to 2) | shadows | highlights | vibrance |
| c6 `cDetail` | sharpen (0 to 1.5) | clarity (0 when below 0.001) | | |
| c7 `cTintS` | shadow tint rgb | | | amount (0 to 1) |
| c8 `cTintH` | highlight tint rgb | | | amount (0 to 1) |
| c9 `cMixA` | red | yellow | green | cyan |
| c10 `cMixB` | blue | magenta | mixer on | |
| c11 `cVig` | amount (0 to 0.8) | size (0 to 0.95) | W/H | on (amount > 0.001) |
| c12 `cBase` | 1/8-scene width | height | 1 / width | 1 / height |

The filters use c13..c54 and `AdaptPS` c56. The on flags are c13 `cFlagA` (Technicolor 1, Technicolor 2, DPX,
Colorfulness), c14 `cFlagB` (Night Mode, Vintage, Cross-process, Black and white), c15 `cFlagC` (Glow, Halation,
Dreamy), c16 `cFlagD` (Emphasize, Tilt-shift, Prism, Film grain), c17 `cFlagE` (3DFX, CRT, Levels, Filmic pass), c44
`cFlagF` (Tint, Fake HDR), c46 `cFlagG` (Auto exposure, Adaptive sharpening, Color-blind mode) and
c47.z (LUT). The per-filter amounts, colours and camera values (near plane and depth scale for Emphasize) fill
the other registers; their layout is commented in the shader source in `picture.cpp`.

**Diagnostics.** When Picture is on but not applied for 2 s, the card and the log give the reason (`Picture::Problem`):
EndScene does not reach it, the frame boundary does not reach it (another mod took over Present), the back buffer
cannot be read, frames end on another render target, or resources failed (with the format and HRESULT). Other log lines:
`[Picture] On` / `Off`, `[Picture] Applied to the game's picture (WxH)`, `[Picture] Frames now end on device ...` (the
game has two devices, a tiny one first), a status line every minute (passes, passes with the scene copy, size, format,
pass check, settings), the menus-tinted state, and `[Picture] Settings saved: ...`. The Developer page shows the 8-bit /
dither note and the GPU cost.

**Configuration migration.** On first start without `ApexRadiance.toml`, the migration from `S3SS.toml` copies the keys
`Picture::Keys()` lists from `[qol.picture]` (the grade keys; the filters are new and have no older keys). If there is
no `[qol.picture]` but there is a `[qol.hdr]` (settings from before the Picture section existed), it builds
`[qol.picture]` from `[qol.hdr].enabled` and the 12 grade keys `exposure`, `contrast`, `midtones`, `shadows`,
`highlights`, `blacks`, `temperature`, `tint`, `saturation`, `vibrance`, `deband`, `sharpen`
([apex_config.cpp](../../apex_config.cpp)).

**Game code.** None patched. Picture relies only on the frame's draw pattern (at least 20 depth-tested back-buffer
draws, the bloom composite as one 2-primitive strip right after the scene, the UI drawn with the depth test off).

## Rejected approaches

- Copying the scene at the first depth-off draw: interiors draw depth-off mid-scene, so 98.7% of the screen was treated
  as UI.
- Draw hooks below the features that skip draws: they missed Night Lighting's replaced draws.
- Copying before the bloom strip: bloom would be treated as UI and stay ungraded.
- Grading 8-bit data without a dither, or with a temporal dither: banding, or shimmer.
- Random-sample deband: replaced by fixed rings with a threshold, scene only.
- A soft UI mask (`difference x 64`): the strong filters half-filtered soft UI edges and text shadows; replaced by the
  4 to 24-level mask with the veil ratio.
- Sun rays, Cartoon and Light leaks filters: removed from the Filters tab before release.
- Relight (up to 4 lights of the player's own, fixed in the world, with screen-space shadows): removed before release.

Details in [history](../history/picture-filters.md).

## See also

- [Validation](../validation/picture-filters.md)
- [History](../history/picture-filters.md)
- [Banding Fix](banding-fix.md)
- [Removed features: HDR output](../removed-features.md)

## Local Color UI candidate (2026-10-07)

This is a test candidate, not a released version. Overview presents the existing master switch, GPU cost and
comparison tools once, followed by five clickable group rows. There are no profiles in this overview. Per-tab
headers control only their group; the global master still gates all Color processing. Stored values are not reset
when a group is disabled. Basic, Tones, Color and Detail neutralise only their respective shader inputs; Filters
disables the 25 filter branches. Banding Fix's deband continues independently of these switches.

| Menu label | TOML key in `[qol.picture]` | Default | Behaviour |
|---|---|---|---|
| Basic | `basic_enabled` | true | Brightness, contrast, saturation, temperature and sharpness |
| Tones | `tones_enabled` | true | Midtones, shadows, highlights and blacks |
| Color | `color_enabled` | true | Tint, vibrance, split toning and mixer |
| Detail | `detail_enabled` | true | Clarity and vignette |
| Filters | `filters_enabled` | true | All filter branches, retaining individual switches and parameters |
| Filter shortcut | `filter_shortcuts.<stable filter key>` | absent | Array of canonical Windows virtual-key integers; only on/off, never intensity |

Group keys absent from older files default to true, preserving their existing appearance. Shortcut arrays are
optional, empty by default, and saved with Picture settings in config, Color profiles and undo snapshots.
Malformed arrays and modifier-only bindings are ignored. Right-clicking a filter, or its ellipsis button, offers
Assign/Change and Remove shortcut. The editor records simultaneously held keys, including letter chords such as
A+S and optional Ctrl/Shift/Alt. Save commits; Cancel or Esc leaves the previous binding intact. Duplicate bindings
and existing Apex actions are refused. Native F10 remains the game's UI command. No shortcut is supplied by default.
Key-down events are queued for the render thread, repeats do not toggle again, and typing in Apex text fields or the
recognised cheat console is protected. A shortcut never enables Picture or the Filters group automatically.
Single letters can also be game commands; choose combinations accordingly. The first key of a multiple-letter
chord may reach the game before the complete chord is recognised; Windows-level shortcuts also remain Windows-owned.

Tags use the native switch geometry, an 8-unit gap and matching vertical centres. Very long chords shorten the
visible tag at token boundaries, with the full chord on hover. Expanded sliders retain every existing label,
range, default, reset and setter. Search traverses collapsed filter controls without opening them.

The menus-tinted diagnostic now also accepts a successful pre-UI Color boundary as evidence of scene isolation,
and does not diagnose the main menu as an active-world colour failure. The warning is retained for the actual
missing-isolation fallback; it is shown once on Overview, not repeated on every adjustment tab.

The Overview card uses the tab's title and remains the master Color switch. While it is off, overview/group-header
switches display off and are disabled; saved group preferences are not overwritten, and return when the master is on.
Filter disclosure buttons point down when closed and up when open. The shortcut editor uses the native modal card
header, identifies the chosen filter, shows keys in a full-width read-only field and aligns Cancel then Save on the
right. Its width is constrained across frames; Cancel, the close icon and Esc retain the previous shortcut.

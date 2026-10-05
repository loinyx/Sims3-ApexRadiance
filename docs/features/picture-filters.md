# Picture filters

Picture filters adjust how the game world looks: brightness, contrast, saturation, colour temperature, tone zones,
film-style split toning, a per-colour mixer, sharpness, clarity and a vignette. Only the 3D scene is graded; the game's
menus, pie menus, tooltips, the HUD and the Apex menu keep their own colours. Grading adds no banding of its own. A
before / after split and a hold-to-compare button show the difference at any time. In the menu the feature is the
*Picture* card of the **Color** page.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (present in the first version in this repository) |
| Default | Off |
| Menu | Image > Color (tabs Basic, Tones, Color, Detail; *Smooth gradients* on the Banding tab); Overview > Image > Picture |
| Configuration | `[qol.picture]` in `ApexRadiance.toml`; compare key `[ui] picture_compare_key` |
| Source | [`features/picture.cpp`](../../features/picture.cpp), [`features/picture.h`](../../features/picture.h), [`framework/d3d9_bootstrap.cpp`](../../framework/d3d9_bootstrap.cpp) (fire points) |

## The problem

The game offers no colour grading. A filter applied to the whole final frame (as ReShade does) also changes the
interface, which makes text and icons look wrong. Grading 8-bit data also turns smooth gradients into visible steps, and
the game's own DXT textures (5 to 6-bit colour endpoints) and 8-bit light maps already band.

## How Apex Radiance solves it

Apex Radiance copies the back buffer at the moment the game switches from drawing the 3D scene to drawing its
interface. At the end of the frame it grades the back buffer in one full-screen pass and keeps every pixel that differs
from that scene copy (the interface) unchanged. The grade runs in linear light and is re-quantised to 8 bits with a fixed
dither below one step.

1. **Find the end of the scene.** Picture counts depth-tested draws into the back buffer. At every switch from
   depth-tested to depth-off drawing (after at least 20 scene draws) it copies the back buffer; the last copy of the
   frame wins. When the switch is the game's bloom composite (a 2-primitive triangle strip right after the scene), the
   copy is taken after the bloom.
2. **End of frame.** If the frame ended on the scene (no game UI after it), the copy is taken just before the Apex
   overlay.
3. **The pass** runs at the game's EndScene, after the overlay: copy the finished frame, then one shader decodes,
   grades and encodes each scene pixel and writes UI pixels back unchanged.

## Settings

All values are stored in `[qol.picture]`. Sliders apply live while dragging and save when released.

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Picture (card switch) | `enabled` | bool | off | | Turns the grade on; the rows stay visible, greyed, while off |
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
| Banding > Smooth gradients | `deband` | float | 100% | 0 to 200% | Deband filter; follows the Banding Fix switch, not Picture's (see [banding-fix.md](banding-fix.md)) |

Missing keys keep their defaults. *Film tones* and *Color mixer* are collapsed *Advanced* groups.

## Compatibility and interactions

- **Post-scene effects** (Ambient Occlusion, Edge Smoothing, Depth Blur) run at the end of the scene, before Picture's
  copy (Picture's draw hooks run at priority 10, after the post-scene trigger's `Priority::First` hooks; with the game
  UI hidden the post-scene fallback runs before `BeforeOverlay`). Their pixels are inside both the frame and the scene
  copy, so they are graded like the rest of the scene.
- **Banding Fix:** *Smooth gradients* follows the Banding Fix switch. With Picture off and the Banding Fix on, the pass
  runs with only the deband (every other control neutral), and never in a frame without a scene copy, so it cannot
  smooth the game's menus.
- **The game's Edge Smoothing (MSAA):** when the game does not draw its scene straight into the back buffer, there is no
  scene copy. The whole picture, menus included, is then graded, and after 2 s the card shows: "Color also tints the
  game's menus here ... turning off the game's own Edge Smoothing (Options > Graphics) usually fixes it".
- **Night Lighting** replaces some draws and returns `Skip`; Picture's draw hooks run before any feature that may skip a
  draw (`Normal` priority), so they see every scene draw. Re-issued draws are counted twice (harmless). The pond pass is
  marked as an internal pass and ignored.
- **Apex menu:** drawn after `BeforeOverlay`, so it is always treated as UI, including in frames with no game UI.
- **Filtered screenshots:** the screenshot shortcut reads the back buffer at Present, after the Picture pass. With the
  menu open, the bootstrap runs Picture once and then fires `filteredSceneBeforeOverlay`, before the menu draws. See
  [bug-reports.md](bug-reports.md#screenshot-settings).
- **Compare with the game** (shortcut, Ctrl+Shift+F10 with the default function-key preset) turns Picture off with the
  other effects, without saving. A configuration saved meanwhile still stores Picture as on.
- **Profiles** carry `[qol.picture]` in their Color part, together with the Banding Fix. Applying a profile goes through
  `SetParams`.
- **All effects** (Overview) includes Picture.
- **Resolution changes and borderless modes:** sizes come from the back-buffer description, not the window.
- **Device reset** (alt-tab, resolution change): resources are released before the reset and recreated at the next
  pass.

## Limitations

- No scene copy, no separation: frames with fewer than 20 depth-tested back-buffer draws are graded as a whole. Loading
  frames are skipped until shader precompilation completes.
- The first frame after turning Picture on is not graded (its hooks register in that frame).
- If the game draws depth-tested geometry after it started its UI, the next depth-off draw retakes the copy and the UI
  drawn before it counts as scene (inferred from the "last copy wins" rule; no case observed).
- The pass runs once per frame, at the first EndScene where render target 0 is the back buffer. Draws after it in the
  same frame are not graded (inferred; not observed).
- The UI mask is soft (x64), so faint translucent UI over the scene is partly graded.
- Deband and sharpening sample neighbours from the finished frame; near UI edges the deband threshold excludes most UI
  pixels and sharpening clamps to the neighbour range.
- 8-bit output: Brightness and Highlights pushed above white clip at the encode.
- With Picture on and every slider neutral, the image still changes slightly (the dither and the default deband).

## Technical reference

**Fire points** (`framework/d3d9_bootstrap.cpp`, inside the game's EndScene):

1. `RenderCallbacks::endSceneBeforeOverlay` (the post-scene fallback among others).
2. `Picture::BeforeOverlay`: takes the copy when the effective grade is on, the frame is pending, at least 20 scene draws
   occurred and the frame ended on the scene (`lastWasScene || copyAfterStrip`).
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
  call); resets `frameReady`, `sceneDraws`, `lastWasScene`, `copyAfterStrip`, `sceneCopied`.
- `OnGameDraw`: ignores internal passes and draws not into the back buffer. Depth on: `sceneDraws++`, `lastWasScene`.
  Depth off with `sceneDraws >= 20` right after a scene draw: a `D3DPT_TRIANGLESTRIP` `DrawPrimitive` of exactly 2
  primitives is the bloom composite (copy at the next back-buffer draw); anything else copies now
  (`StretchRect(backbuffer -> sceneSurf, D3DTEXF_NONE)`).

**Pass** (`Picture::OnEndScene`): skipped while precompile is incomplete, while held for compare, when RT0 is not the
back buffer, or (Picture off, deband only) without a scene copy. Then:

1. Read previous timestamps; begin a new set (4 rotating sets, `ms = 0.9 ms + 0.1 v`).
2. `StretchRect(backbuffer -> frameSurf, D3DTEXF_NONE)`.
3. If clarity is not 0: build the 1/2, 1/4, 1/8 chain with linear `StretchRect`s from `sceneSurf` (or `frameSurf`
   without a copy).
4. Save 12 render states, 3 samplers x 6 sampler states and textures, PS, VS, declaration / FVF, stream 0, PS constants
   c0..c12 and the viewport. Neutral state (Z, blend, alpha test, stencil, scissor, fog, sRGB write, clip planes off; cull
   none; colour write 0xF).
5. Bind s0 = frame (point), s1 = scene (point), s2 = 1/8 scene (bilinear); set the shader, then the constants; draw one
   full-screen `DrawPrimitiveUP` quad (FVF `XYZRHW | TEX1`, -0.5 pixel offset) into the back buffer, reading the frame
   copy.
6. Restore everything (stream 0 explicitly) and end the timestamps.

After each settings change the next 3 passes read the constants and bound shader back and log
`[Picture] Pass check: ...` when another mod changed them on the way.

**Resources** (`InitResources`, all `D3DPOOL_DEFAULT` render targets in the back buffer's format, normally
`D3DFMT_A8R8G8B8`): `frameTex`, `sceneTex` (back-buffer size), `chainTex[0..2]` (ceil halvings). Shader `PicturePS`
("picture.hlsl", ps_3_0, `D3DCOMPILE_OPTIMIZATION_LEVEL3`) compiled at start-up on a background thread
(`framework/shader_cache.h`); creation failure is permanent until restart. Log: `[Picture] Resources ready (WxH,
format N)`.

**Shader** (`PicturePS`), per pixel:

1. **UI mask:** `ui = sceneCopied ? saturate(max_channel(|frame - scene|) x 64) : 0`. A difference of 1/64 or more is full
   UI; without a copy the whole frame is graded.
2. **Deband** (scene pixels, `ui < 0.5`): average with samples on two rings of 8 fixed directions (ring 2 rotated
   22.5 degrees), radii `12 x H/2160` and `32 x H/2160` px, accepting only samples whose max channel difference is
   below `deband x 6/255` (gamma-encoded). Deterministic.
3. **Sharpen** (scene pixels): `fs + s x (fs - mean of 4 neighbours)`, clamped to `[min(neighbours, fs),
   max(neighbours, fs)]`.
4. **Decode** gamma 2.2: `pow(max(c, 0), 2.2)`.
5. **Clarity** (scene pixels): `r = log2(Lpixel / Lbase)`, `Lbase` from a cubic B-spline sample of the 1/8 scene (4
   bilinear taps); `g *= exp2(clarity x r / (1 + r^2) x 4e(1 - e))`, `e = saturate(Lpixel^(1/2.2))`. Strong edges get
   almost nothing.
6. **Exposure and white balance:** `g *= exp2(EV) x wb.rgb`. White balance: red `1 + 0.08t`, blue `1 - 0.08t`, green
   `1 - 0.06 tint`, normalised by their Rec.709 luminance.
7. **Contrast:** `g = 0.18 x (g / 0.18)^contrast`.
8. **Tone zones** on luminance (0.2126, 0.7152, 0.0722), hue kept: `Lm = L < 1 ? L^(1/midtones) : L`;
   `Lm *= (1 + 0.5 shadows (1 - smoothstep(0, 0.25, Lm))) x (1 + 0.3 highlights smoothstep(0.3, 1, Lm))`; `g *= Lm / L`.
9. **Blacks:** `b = blacks x 0.02`; `b >= 0: max(g - b, 0) / (1 - b)`, else `g (1 + b) - b`.
10. **Split toning:** `e = saturate(L^(1/2.2))`; `g *= lerp(1, tintS, amountS (1 - smoothstep(0, 0.55, e))) x
    lerp(1, tintH, amountH smoothstep(0.45, 1, e))`. Tints come from `SplitToneColour` on the CPU: the fully saturated
    hue colour `c`, then `1 + 0.5 (c - lum(c))`, whose luminance is exactly 1.
11. **Vibrance:** `sat0 = (max - min) / max`; `g = max(lerp(L, g, 1 + vibrance (1 - sat0)), 0)`.
12. **Saturation and mixer:** `sat = saturation x (mixer on ? MixerSaturation(g) : 1)`; `g = max(lerp(L, g, sat), 0)`.
    `MixerSaturation`: hue 0..6 on gamma-2.2 values, six triangular bands of width 1 whose weights sum to 1; greys
    (sat < 1e-5) get 1. The mixer is on when any value differs from 1 by more than 0.001.
13. **Vignette:** `q = (uv - 0.5) x (W/H, 1)`, `r = |q| / |(W/H, 1) x 0.5|`, `v = smoothstep(size, 1, r)`,
    `g *= 1 - amount x v^2 (3 - 2v)`.
14. **Encode:** `o = pow(saturate(g), 1/2.2) + (IGN(px) - 0.5) / 255`,
    `IGN = frac(52.9829189 x frac(dot(px, (0.06711056, 0.00583715))))`, the same pattern every frame. Compare: the left
    half `o = frame`, the divider red.
15. Output `lerp(o, frame, ui)`.

**Constants** (c0..c12):

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

**Diagnostics.** When Picture is on but not applied for 2 s, the card and the log give the reason (`Picture::Problem`):
EndScene does not reach it, the frame boundary does not reach it (another mod took over Present), the back buffer
cannot be read, frames end on another render target, or resources failed (with the format and HRESULT). Other log lines:
`[Picture] On` / `Off`, `[Picture] Applied to the game's picture (WxH)`, `[Picture] Frames now end on device ...` (the
game has two devices, a tiny one first), a status line every minute (passes, passes with the scene copy, size, format,
pass check, settings), the menus-tinted state, and `[Picture] Settings saved: ...`. The Developer page shows the 8-bit /
dither note and the GPU cost.

**Configuration migration.** On first start without `ApexRadiance.toml`, the migration from `S3SS.toml` copies the keys
`Picture::Keys()` lists from `[qol.picture]`. If there is no `[qol.picture]` but there is a `[qol.hdr]` (settings from
before the Picture section existed), it builds `[qol.picture]` from `[qol.hdr].enabled` and the 12 grade keys
`exposure`, `contrast`, `midtones`, `shadows`, `highlights`, `blacks`, `temperature`, `tint`, `saturation`,
`vibrance`, `deband`, `sharpen` ([apex_config.cpp](../../apex_config.cpp)).

**Game code.** None patched. Picture relies only on the frame's draw pattern (at least 20 depth-tested back-buffer
draws, the bloom composite as one 2-primitive strip right after the scene, the UI drawn with the depth test off).

## Rejected approaches

- Copying the scene at the first depth-off draw: interiors draw depth-off mid-scene, so 98.7% of the screen was treated
  as UI.
- Draw hooks below the features that skip draws: they missed Night Lighting's replaced draws.
- Copying before the bloom strip: bloom would be treated as UI and stay ungraded.
- Grading 8-bit data without a dither, or with a temporal dither: banding, or shimmer.
- Random-sample deband: replaced by fixed rings with a threshold, scene only.

Details in [history](../history/picture-filters.md).

## See also

- [Validation](../validation/picture-filters.md)
- [History](../history/picture-filters.md)
- [Banding Fix](banding-fix.md)
- [Removed features: HDR output](../removed-features.md)

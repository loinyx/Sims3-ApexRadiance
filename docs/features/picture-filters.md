# Picture filters

> Colour and image filters for the game's 3D scene: exposure, contrast, tone zones, white balance, saturation,
> vibrance, split toning, per-hue colour mixer, gradient smoothing (deband), sharpening, clarity and vignette. They run
> as one full-screen pixel-shader pass at the end of the frame, on the normal 8-bit SDR back buffer (gamma 2.2 decode,
> grade in linear light, gamma 2.2 encode plus a fixed interleaved-gradient-noise dither). The game UI and the S3SS
> overlay are masked out and keep their own pixels. Status: **working** (live feature of the standalone). Built in both
> flavours (public and development); no developer-only controls.
>
> In the combined build (tag `combined-final`, commit 45e36e2) the filters live inside `HdrOutput`
> (`hdr_output.cpp/.h`) and share its pass with HDR output. HDR output and Native HDR are **removed** from the standalone
> (see [../removed-features.md](../removed-features.md)); Picture is the only part that stays and must be carved out into
> its own SDR-only module. Section "Carve-out for the standalone" lists exactly what is shared.

## Purpose
- Give the user a colour grade of the scene without touching menus, pie menus, tooltips, the HUD or the S3SS/Apex
  overlay (grading the UI makes text and icons look wrong).
- Remove the banding the game itself produces: DXT textures (5-6 bit colour endpoints) and 8-bit light maps turn smooth
  gradients into steps. "Smooth gradients" (deband) targets exactly those steps.
- Do this without adding banding of its own: every graded pixel is re-quantised to 8 bits with a fixed dither below one
  8-bit step.

History: the grading controls were first added as part of HDR output on 28/09 ("HDR (28/09)" in NOTAS-ILUMINACAO.md:
exposure, contrast, blacks, expansion start, temperature, deband). The same evening they were split into their own
"Picture" section that works with or without HDR ("HDR nativo e saida HDR (28/09, noite)"): config table
`[qol.picture]`, old grading values migrated from `[qol.hdr]`.

## User-facing settings

Display tab, collapsing header **"Picture"** (default open), right after the HDR header (combined build) and before Edge
Smoothing. Header tooltip (`gui.cpp`, Display tab, `ApexDescriptionTooltip`): "Colour and image filters for the game's
scene, with or without HDR ... Credits: @loinyx" (the "with or without HDR" wording must change in the standalone).
All controls are drawn by `HdrOutput::RenderPictureUI()` (`hdr_output.cpp`, approx. 1666-1753).

Saved in the TOML table `[qol.picture]` of `S3SS.toml` (combined build: `Documents\Electronic Arts\<localized The Sims 3
folder>\S3SS\S3SS.toml`, `ConfigPaths::GetConfigPath`). The standalone keeps the table name `[qol.picture]` in its own
`ApexRadiance.toml` under `...\Apex Radiance\` (PLANO-SEPARACAO.md, "Config schema"). Sliders save when the drag ends
(`ImGui::IsItemDeactivatedAfterEdit` -> `SetPicture(q, true)` -> `ConfigStore::SaveAll()`); checkboxes and buttons save
immediately; values apply live every frame while dragging.

| UI label (group) | TOML `[qol.picture]` key | Type | Default | UI range | Shader-side clamp / mapping | Notes |
|---|---|---|---|---|---|---|
| Enabled | `enabled` | bool | false | - | - | Everything below is disabled (greyed) while off. Status text when on: "Running on the SDR image (8-bit, dithered: no added banding)." plus "GPU cost: x.xx ms per frame". |
| Before / after | (not saved) | bool | false | - | `cLook.w` | Left half of the screen = the frame as it came in, right half filtered, 1-pixel red divider at x = 0.5. Always false after load. The checkbox stays clickable with Enabled off, but in SDR the pass does not run then, so it has no effect. |
| Exposure (EV) | `exposure` | float | 0.0 | -2..2 | clamp -3..3, `cGrade.x = exp2(EV)` | Linear gain in stops. |
| Contrast | `contrast` | float | 1.0 | 0.6..1.5 | clamp 0.5..1.8, `cGrade.y` | Power curve around mid grey 0.18 (linear). |
| Saturation | `saturation` | float | 1.0 | 0..1.6 | clamp 0..2, `cLook.x` | Lerp from luminance; multiplied by the colour mixer value when the mixer is not neutral. |
| Temperature | `temperature` | float | 0.0 | -1..1 | clamp -1..1, x 0.08 | Red gain 1 + 0.08t, blue 1 - 0.08t, normalised to keep white's luminance (`cWb.rgb`). |
| Smooth gradients | `deband` | float | **1.0** | 0..2 | clamp 0..2; threshold `deband x 6/255` (encoded units) | Since 30/09 on the Color page's Banding tab and gated by the Banding Fix switch (`SceneDither`), not by Picture: with Picture off the pass runs with only the deband (`Effective()` in picture.cpp: every other control neutral), skipped in frames without a scene copy. Reset Picture keeps it. 0 = off (`cDeband.w`). |
| Sharpening | `sharpen` | float | 0.0 | 0..1.5 | clamp 0..1.5, `cColor.z` | 4-neighbour unsharp, clamped to the neighbours' range (no halos). |
| Midtones (Advanced > Tones) | `midtones` | float | 1.0 | 0.6..1.6 | clamp 0.5..2; `cTone.x = 1/midtones` | Power on luminance below 1: black and white stay. |
| Shadows (Advanced > Tones) | `shadows` | float | 0.0 | -1..1 | `cTone.y` | Up to +/-50% gain below luminance ~0.25. |
| Highlights (Advanced > Tones) | `highlights` | float | 0.0 | -1..1 | `cTone.z` | Up to +/-30% gain from luminance 0.3 to 1. In SDR anything pushed past 1 clips at the encode. |
| Blacks (Advanced > Tones) | `blacks` | float | 0.0 | -1..1 | x 0.02 (`cGrade.z`) | + deepens (subtract up to 2% of white, rescale), - lifts (add up to 2%). |
| Tint (Advanced > Colour) | `tint` | float | 0.0 | -1..1 | x 0.06 on green | + = magenta (less green), - = green. |
| Vibrance (Advanced > Colour) | `vibrance` | float | 0.0 | -1..1 | `cTone.w` | Saturation scaled by (1 - current saturation). |
| Shadow colour (Split toning) | `shadow_hue` | float | 215.0 | 0..360 deg | `cTintS.rgb` via `SplitToneColour` | A colour swatch follows the slider. |
| Shadow amount (Split toning) | `shadow_tint` | float | 0.0 | 0..1 | clamp 0..1, `cTintS.w` | 0 = off. |
| Highlight colour (Split toning) | `highlight_hue` | float | 40.0 | 0..360 deg | `cTintH.rgb` | |
| Highlight amount (Split toning) | `highlight_tint` | float | 0.0 | 0..1 | clamp 0..1, `cTintH.w` | 0 = off. |
| Reds / Yellows / Greens / Cyans / Blues / Magentas (Colour mixer) | `mixer` | array of 6 floats | [1,1,1,1,1,1] | 0..2 each | clamp 0..2, `cMixA`, `cMixB.xy`; `cMixB.z` = on when any differs from 1 by > 0.001 | Saturation per hue family; tooltip on Greens: "grass and leaves. Lower = less neon foliage." |
| Clarity (Detail) | `clarity` | float | 0.0 | -1..1 | clamp -1..1, `cColor.w`; 0 when abs < 0.001 | Needs the 1/8-size scene chain (built only when clarity != 0). |
| Amount (Vignette) | `vignette` | float | 0.0 | 0..0.8 | clamp 0..0.8, `cVig.x`; on when > 0.001 | |
| Size (Vignette) | `vignette_size` | float | 0.5 | 0..0.95 | clamp 0..0.95, `cVig.y` | Radius (0 centre, 1 corner) where darkening starts. |

Defaults are the member initialisers of `struct PictureParams` (`hdr_output.h`); `LoadFromToml` falls back to the
current member value for every missing key, so a partial table keeps defaults for the rest.

### Migration from `[qol.hdr]` (`HdrOutput::LoadFromToml`, `ReadGrade`)
- If `[qol.picture]` exists: read it (`enabled`, the 12 grade keys through `ReadGrade`, then `shadow_hue`,
  `shadow_tint`, `highlight_hue`, `highlight_tint`, `clarity`, `vignette`, `vignette_size`, `mixer[0..5]`).
- Else, if `[qol.hdr]` exists (config written before the Picture section existed): `enabled = [qol.hdr].enabled` and
  `ReadGrade([qol.hdr])` reads the 12 shared grade keys: `exposure`, `contrast`, `midtones`, `shadows`, `highlights`,
  `blacks`, `temperature`, `tint`, `saturation`, `vibrance`, `deband`, `sharpen`. So a user who had HDR on gets Picture
  on with the same grade.
- `SaveToToml` never writes grade keys into `[qol.hdr]`, so after the first save the old keys disappear from `[qol.hdr]`
  and `[qol.picture]` is authoritative.
- The config can be read before the regular config load: `HdrOutput::BeforeCreateDevice` parses the whole TOML when the
  game creates its device before S3SS loaded the config (same situation as `BorderlessWindow::PeekEnabledEarly`); that
  early `LoadFromToml` also fills Picture. For SDR-only Picture there is no need to read early (nothing is decided at
  device creation), see Carve-out.

## How it works

### Per frame (combined build, SDR, `m_active == false`)
1. **Frame boundary** (`OnFrameBoundary`, registry Present hook, `Priority::First`): remember the back buffer surface
   (identity only), reset `sceneDraws`, `lastWasScene`, `copyAfterStrip`, `sceneCopied`, `bbDrawIndex`, set
   `frameReady = true`.
2. **Track RT0** (`RegisterSetRenderTarget`, `Priority::First`): `curRT0` = the surface set on index 0.
3. **Classify every game draw** (`OnGameDraw` from the registry DIP and DP hooks, both `Priority::First`):
   - draws marked `DepthShare::InternalPass()` (e.g. the lake lamp pass of lot_light_bridge, which turns ZENABLE off)
     and draws not into the back buffer are ignored;
   - `ZENABLE != FALSE` -> a scene draw: `sceneDraws++`, `lastWasScene = true`, `copyAfterStrip = false`;
   - `ZENABLE == FALSE` with `sceneDraws >= 20`, right after a scene draw (a depth-on -> depth-off transition):
     - if it is a `D3DPT_TRIANGLESTRIP` `DrawPrimitive` of exactly 2 primitives, it is the game's bloom composite
       (FrameCapture 24/09 #977: one full-screen DP strip right after the scene): set `copyAfterStrip` and copy after
       it, at the next back-buffer draw;
     - otherwise `CopyScene` now: `StretchRect(backbuffer -> sceneSurf, D3DTEXF_NONE)`, `sceneCopied = true`.
   - The copy is **retaken at every** depth-on -> depth-off transition; the last one of the frame wins.
4. **Before the S3SS overlay** (`HdrOutput::BeforeOverlay`, called from `HookedEndScene` right after
   `RenderCallbacks::endSceneBeforeOverlay`, `d3d9_hook.cpp` approx. 166-167): if Picture is on, the frame has had
   >= 20 scene draws and it ended on the scene (`lastWasScene || copyAfterStrip`), i.e. the game drew no UI after the
   scene, take the copy now so the overlay counts as UI.
5. S3SS draws its ImGui overlay (its draws go through the same hooked DIP; they are depth-off, after the copy: UI).
6. **The pass** (`HdrOutput::OnEndScene`, called from `HookedEndScene` after the overlay, just before
   `original_EndScene`, `d3d9_hook.cpp` approx. 237):
   1. `s_lampGain = 1`, `HdrNative::Update(dev, false)` (both HDR-only, removed in the standalone).
   2. Return if Picture is off (`m_gpuMs = -1`).
   3. `RegisterHooks(dev)` on the first call (so the first frame after enabling has no scene copy: whole frame = UI =
      unchanged; the hooks are never unregistered afterwards).
   4. Once per frame (`frameReady`); only when RT0 is the back buffer; `InitResources` on first use or after a Reset.
   5. If the sky-boost depth request was held (HDR only), release it (`DepthShare::Request(false)`).
   6. Read the previous GPU timings; issue a timestamp begin (4 rotating query sets).
   7. `StretchRect(backbuffer -> frameSurf, D3DTEXF_NONE)`: the finished frame (scene + UI + overlay).
   8. If clarity != 0: build the 1/2, 1/4, 1/8 chain with linear `StretchRect`s from `sceneSurf` when the copy exists
      (no UI in the local average), else from `frameSurf`.
   9. Save the device state it touches (12 render states, 6 samplers x 6 sampler states and textures, PS, VS, vertex
      declaration / FVF, stream 0, PS constants c0..c15, viewport), set a neutral state (Z off, blending off, cull none,
      scissor off, fog off, sRGB write off, clip planes off, colour write 0xF), bind textures, upload c0..c15, draw one
      full-screen quad into the back buffer with `DrawPrimitiveUP` (FVF `XYZRHW|TEX1`, -0.5 pixel offset). The frame is
      read from `frameTex`, so the pass writes the back buffer in place.
   10. Restore every saved state (`SetStreamSource` explicitly: `DrawPrimitiveUP` clears stream 0), issue the timestamp end.
7. `original_EndScene`, then Present.

### Render targets and textures (`InitResources`)

| Resource | Size | Format | Filter when sampled | Use |
|---|---|---|---|---|
| `frameTex` / `frameSurf` | back buffer | back buffer format: `D3DFMT_A8R8G8B8` (21), verified in `S3SS_LOG.txt` of 28/09: "[HDR] Resources ready (3840x2160, format 21)" with HDR off | point, sampler s0 | the finished frame |
| `sceneTex` / `sceneSurf` | back buffer | same | point, s1 | the scene before the UI |
| `chainTex[0..2]` | ceil halvings: 1/2, 1/4, 1/8 | same | bilinear, s2 (only [2]) | clarity's local average (cubic B-spline from 4 bilinear taps) |
| `ps` | - | ps_3_0 compiled at runtime with `D3DCompile` (`D3DCOMPILE_OPTIMIZATION_LEVEL3`, entry `HdrPS`) | - | the pass |
| timestamp queries x4 sets | - | `TIMESTAMPDISJOINT`, `TIMESTAMP` x2, `TIMESTAMPFREQ` | - | GPU cost, smoothed `ms = 0.9 ms + 0.1 v` |

**Standalone (`features/picture.cpp`, 2026-09-28):** `PicturePS` ("picture.hlsl", ps_3_0, O3) is compiled at start-up on
a background thread (`framework/shader_cache.h`, [architecture 4.6](../architecture.md#shader-precompile));
`InitResources` only creates the shader object from that bytecode. Not tested in game yet.

All targets are `D3DPOOL_DEFAULT` render-target textures: released in `HdrOutput::BeforeReset` (`ReleaseResources`,
called from `HookedReset` before the original Reset) and recreated lazily at the next pass. The compiled shader is kept
across Resets (`compileTried` stays true; a compile failure is permanent until restart and logs "[HDR] Shader failed to
compile: ..."). In SDR the HDR extras (glow, limiter targets, aux shaders) are not created: they need
`D3DFMT_A16B16G16R16F`.

### The shader (SDR path of `HdrPS`, `kShaderSource` in `hdr_output.cpp`)
Samplers: s0 frame (point), s1 scene (point), s2 1/8 scene (bilinear). s3..s5 are HDR-only and bound to null in SDR.
In order, per pixel:
1. **UI mask.** `ui = sceneCopied ? saturate(max_channel(|frame - scene|) x 64) : 1`. A difference of 1/64 or more is
   full UI; smaller differences blend (1/255 gives ui = 0.25). Without a scene copy the whole frame is UI and the output
   equals the input.
2. **Deband** (scene pixels, `ui < 0.5`, deband on): average the pixel with the samples on two rings of 8 fixed
   directions (ring 1 rotated 22.5 deg), radii `12 x H/2160` and `32 x H/2160` pixels (chosen at 4K), taking only
   samples whose max channel difference to the pixel is below `deband x 6/255` (gamma-encoded units). Deterministic,
   no noise, edges and texture detail above the threshold are left alone.
3. **Sharpen** (scene pixels, sharpen > 0): `fs + s x (fs - mean of the 4 direct neighbours)`, clamped to
   `[min(neighbours, fs), max(neighbours, fs)]`.
4. **Decode** gamma 2.2 (`pow(max(c,0), 2.2)`; in SDR `cLook.y` is forced to 0, so the sRGB piecewise option of HDR is
   never used).
5. **Clarity** (scene pixels, clarity != 0): `r = log2(Lpixel / Lbase)` with `Lbase` the luminance of the B-spline
   sample of the 1/8 scene; `g *= exp2(clarity x r / (1 + r^2) x 4 e (1 - e))`, `e = saturate(Lpixel^(1/2.2))`: strong
   edges (large `|r|`) get almost nothing, only midtones are affected.
6. **Exposure and white balance:** `g = g x exp2(EV) x wb.rgb`.
7. **Contrast:** `g = 0.18 x (g / 0.18)^contrast`.
8. **Tone zones** on luminance (Rec.709 weights 0.2126/0.7152/0.0722), hue kept: `Lm = L < 1 ? L^(1/midtones) : L`;
   `Lm *= (1 + 0.5 shadows (1 - smoothstep(0, 0.25, Lm))) x (1 + 0.3 highlights smoothstep(0.3, 1, Lm))`; `g *= Lm/L`.
9. **Blacks:** `b = blacks x 0.02`; `b >= 0: max(g - b, 0)/(1 - b)`, else `g (1 + b) - b`.
10. **Split toning:** `e = saturate(L^(1/2.2))`; `g *= lerp(1, tintS, amountS (1 - smoothstep(0, 0.55, e))) x
    lerp(1, tintH, amountH smoothstep(0.45, 1, e))`. The tint colours come from `SplitToneColour` (CPU): the fully
    saturated hue colour `c` (`HueColour`), then `1 + 0.5 (c - lum(c))`, whose luminance is exactly 1, so brightness stays.
11. **Vibrance:** `sat0 = (max - min)/max`; `g = max(lerp(L, g, 1 + vibrance (1 - sat0)), 0)`.
12. **Saturation and colour mixer:** `sat = saturation x (mixer on ? MixerSaturation(g) : 1)`; `g = max(lerp(L, g, sat), 0)`.
    `MixerSaturation`: hue 0..6 (red, yellow, green, cyan, blue, magenta) computed on gamma-2.2-encoded values, six
    triangular bands of width 1 whose weights sum to 1, weighted sum of the six slider values; greys (sat < 1e-5) get 1.
13. **Vignette:** `q = (uv - 0.5) x (W/H, 1)`, `r = |q| / |(W/H, 1) x 0.5|` (0 centre, 1 corner),
    `v = smoothstep(size, 1, r)`, `g *= 1 - amount x v^2 (3 - 2v)`.
14. **SDR encode** (`cWb.w = 0`): `o = pow(saturate(g), 1/2.2) + (IGN(px) - 0.5)/255` with
    `IGN = frac(52.9829189 x frac(dot(px, (0.06711056, 0.00583715))))`, `px` = integer pixel coordinates. The pattern is
    the same every frame (no temporal noise). Compare mode: left half `o = frame`, the divider pixel column red.
15. Output `lerp(o, frame, ui)`: UI pixels are written back unchanged.

Note: with Picture enabled and every slider neutral, the image still changes slightly (the dither and deband, which
defaults to 1.0).

## Files and functions

| File (combined tree) | Function / item | Role |
|---|---|---|
| `hdr_output.h` | `struct PictureParams` | settings and defaults |
| `hdr_output.h` | `HdrOutput::GetPicture / SetPicture / RenderPictureUI / BeforeOverlay / OnEndScene / BeforeReset / SaveToToml / LoadFromToml / GpuMs` | public surface |
| `hdr_output.cpp` | `kShaderSource` (`HdrPS`, `Deband`, `SampleBase`, `HueSat`, `MixerSaturation`) | the pass shader (shared with HDR) |
| `hdr_output.cpp` | `struct Gpu`, `CopyScene`, `OnGameDraw`, `OnFrameBoundary`, `RegisterHooks` | scene/UI mask machinery (registry hook name "HdrOutput") |
| `hdr_output.cpp` | `CompileShader`, `InitResources`, `ReleaseResources`, `ReadTimings` | resources, compile, GPU timing |
| `hdr_output.cpp` | `HdrOutput::OnEndScene` | the per-frame pass, constant packing c0..c15 |
| `hdr_output.cpp` | `HueColour`, `SplitToneColour`, `HueSwatch`, `Hint` | CPU helpers, UI swatches and tooltips |
| `hdr_output.cpp` | `ReadGrade`, `SaveToToml`, `LoadFromToml` | `[qol.picture]` and migration |
| `d3d9_hook.cpp` | `HookedEndScene` (calls `BeforeOverlay` approx. l.167 and `OnEndScene` approx. l.237), `HookedReset` (`BeforeReset` approx. l.253) | fire points |
| `config/config_store.cpp` | `SaveAll` (approx. l.52), `LoadAll` (approx. l.90) | `HdrOutput` saves/loads `[qol.hdr]` and `[qol.picture]` in one call |
| `gui.cpp` | Display tab (approx. 599-618) | "Picture" collapsing header |
| `d3d9_hook_registry.h/.cpp` | `Priority::First`, `HookAction::Skip` | ordering: a `Skip` stops the remaining hooks of that call |
| `depth_share.h` | `InternalPass()` | lets other patches mark their own depth-off draws |

## Game addresses and patterns
None. Picture works purely at the D3D9 level: it relies on the draw pattern of the game's frame (>= 20 depth-tested
back-buffer draws, bloom composite as one 2-primitive `D3DPT_TRIANGLESTRIP` `DrawPrimitive` right after the scene,
FrameCapture 24/09 #977; UI drawn with ZENABLE off).

## Shader constants (c0..c15 of `HdrPS`, as packed in SDR)

| Reg | x | y | z | w |
|---|---|---|---|---|
| c0 `cNits` | HDR only (paper/80) | HDR only | HDR only | HDR only |
| c1 `cLook` | saturation | 0 (gamma 2.2 forced in SDR) | scene copy valid | compare |
| c2 `cSize` | 1/W | 1/H | W | H |
| c3 `cGrade` | exp2(exposure) | contrast | blacks x 0.02 | (HDR expansion start `knee`, unused in SDR) |
| c4 `cWb` | wbR/wbL | wbG/wbL | wbB/wbL | 0 = SDR output |
| c5 `cDeband` | deband x 6/255 | 12 x H/2160 | 32 x H/2160 | on |
| c6 `cTone` | 1/midtones | shadows | highlights | vibrance |
| c7 `cColor` | 1 (HDR highlight sat) | 0 (HDR gamut) | sharpen | clarity |
| c8 `cTintS` | shadow tint rgb | | | amount |
| c9 `cTintH` | highlight tint rgb | | | amount |
| c10 `cMixA` | red | yellow | green | cyan |
| c11 `cMixB` | blue | magenta | mixer on | 0 |
| c12 `cVig` | amount | size | W/H | on |
| c13 `cBase` | 1/8-scene width | height | 1/width | 1/height |
| c14 `cSky` | 0 in SDR | 0 | 0 | 0 |
| c15 `cCal` | 0 in SDR | | | |

## Interactions
- **Post-scene effects (Ambient Occlusion order 10, Edge Smoothing 20, Depth Blur 30; `post_scene.cpp`).** They run at
  the *first* depth-off back-buffer draw after >= 20 scene draws, draw with `DrawPrimitiveUP` (not hooked, so they are
  invisible to Picture's draw counter) and finish before the UI. Picture copies the scene later (last transition), so
  their results are inside both the frame and the scene copy and are graded like the rest of the scene. Exception
  (inferred from the code, not observed): when no bloom strip follows the scene, Picture's copy and the PostScene
  trigger fire on the same draw, and the order of two `Priority::First` hooks is undefined (`std::sort`, not stable);
  if the copy runs first, pixels changed by Edge Smoothing / Depth Blur differ from the copy by more than 1/64 and are
  treated as UI (left ungraded). Note the two
  trigger rules differ: PostScene fires at the first transition, Picture's copy at the last; in interiors the first
  depth-off draw can be in the middle of the scene (HDR_Diag 28/09), which matters for the post-scene effects, not for
  Picture ([edge-smoothing.md](edge-smoothing.md), [depth-blur.md](depth-blur.md); Ambient Occlusion is removed from the
  standalone, see [../removed-features.md](../removed-features.md)).
- **Night Lighting / lot_light_bridge.** It replaces draws and returns `HookAction::Skip`, which ends the registry's hook
  chain for that draw. Picture's hooks are `Priority::First` so they see every draw (the reason for the 98.7%-UI bug,
  see Pitfalls). Its re-issued draws go through the hooks again and are counted twice as scene draws (harmless).
  Its lake lamp pass is marked `DepthShare::SetInternalPass(true)` (`lot_light_bridge.cpp` approx. 825) and is ignored.
- **S3SS overlay / Apex menu.** Drawn after `BeforeOverlay`, so it is always UI (unchanged pixels), including frames with
  no game UI.
- **Frame Capture (dev tool).** Source of the bloom-strip rule (#977); see [dev-tools/frame-capture.md](dev-tools/frame-capture.md).
- **The game's C screenshot:** when Apex's screenshot shortcut is enabled, it consumes C and saves one filtered PNG
  in the game's Documents `Screenshots` folder. It reads the back buffer at Present, after the scene effects and Picture
  pass; see [bug-reports.md](bug-reports.md#player-screenshots). If the shortcut is disabled, the native C capture's
  inclusion of the filters remains unverified.
- **Game's own Edge Smoothing (MSAA):** unverified for Picture. If the game renders the scene off the back buffer and
  resolves it with a copy, fewer than 20 depth-tested back-buffer draws would mean no scene copy and no filtering
  (inferred from the counter rule, not tested). The README asks for the game's Edge Smoothing off for all post effects.
- **Resolution Spoofer / borderless:** the pass sizes everything from the back buffer description, not the window.
- **Reset (alt-tab, resolution change):** resources released before Reset, recreated at the next frame.

## Known limitations
- No scene copy -> no filtering: main menu / loading screens / any frame with fewer than 20 depth-tested back-buffer
  draws are left untouched (by design: `ui = 1`).
- The first frame after enabling is unfiltered (hooks are registered in that frame).
- If the game draws depth-tested geometry after it started drawing UI, the next depth-off draw retakes the copy and the
  UI drawn before it counts as scene (inferred from the "last copy wins" rule; no case observed).
- The pass runs at the first EndScene of the frame where RT0 is the back buffer (`frameReady` gate); if the game ends
  more than one scene per frame with the back buffer bound, draws after that EndScene are not filtered (inferred; the
  code comment says "the game may end more than one scene", no evidence it happens with RT0 = back buffer).
- Translucent UI over the scene: the mask is soft (x64), so faint UI over the scene is partly graded.
- Deband and sharpen sample `sFrame` (with UI) for neighbours; near UI edges the UI pixels are mostly excluded by the
  deband threshold, sharpen clamps to the neighbour range (no visible issue reported).
- 8-bit output: "Highlights" and "Exposure" above white clip at the encode (`saturate`).
- No SDR diagnostic dump: the HDR_Diag button works only with HDR active (`diag = diagState == 2 && m_active`), and it is
  dev-only.

## Pitfalls and failed approaches
- **98.7% of the screen treated as UI (HDR_Diag_1/2, 28/09).** The first version copied the scene at the first
  depth-off draw after the scene. Interiors have depth-off draws in the middle of the scene, so the copy missed most of
  the lamp light and the mask marked almost everything as UI: grading never applied. Fix (current code): retake the
  copy at every depth-on -> depth-off transition (last wins), with the bloom strip copied after it. Do not go back to
  "first depth-off draw".
- **Hooks at Normal priority missed draws.** lot_light_bridge returns `Skip` and cuts the registry chain, so a lower
  priority Picture hook never saw the replaced scene draws. Keep `Priority::First` for Present, SetRenderTarget, DIP and DP.
- **Bloom composite is part of the scene.** Copying before the 2-primitive strip would leave the bloom outside the scene
  copy (the bloom would then be marked UI and stay ungraded).
- **Banding from grading 8-bit data.** Solved by grading in float and re-quantising with a fixed dither below one 8-bit
  step; no temporal noise (would shimmer).
- **Deband by random sampling** was not used: deterministic rings (2 x 8) with a threshold, scene only (UI keeps sharp
  edges) (NOTAS "HDR (28/09)").
- The combined build's README still lists the grading controls under "HDR > Advanced": stale since the Picture split.

## Testing in game
1. Display tab > Picture > Enabled. Expect "Running on the SDR image (8-bit, dithered: no added banding)." and a
   "GPU cost" line (a value appears after a few frames).
2. **Mask check:** set Saturation to 0. The 3D scene must turn grey while the pie menu, HUD, tooltips and the S3SS/Apex
   menu keep colour. Whole screen coloured = no scene copy (fewer than 20 scene draws, RT0 not the back buffer, or hooks
   not seeing draws: check hook priority). UI grey too = the copy was taken after the UI.
3. Test in an interior at night (the 98.7% case) and outdoors with bloom.
4. Before / after: left half original, red line in the middle.
5. Deband: a sky gradient or a dim interior wall with visible steps should smooth at 1.0 without blurring edges.
6. Alt-tab / change resolution: filtering continues after the Reset; log shows "[HDR] Resources ready (WxH, format N)" again.
7. `S3SS_LOG.txt`: "[HDR] Resources ready ..." on first use; "[HDR] Shader failed to compile: ..." on failure.

## Carve-out for the standalone
What is shared with HDR in `hdr_output.cpp/.h` and what to do with it:

| Piece | Keep / drop | Notes |
|---|---|---|
| `PictureParams` | keep | move to its own header |
| `HdrPS` | keep the Picture part | drop `cNits`, `cSky`, `cCal`, `cLook.y` (gamma fixed 2.2), `cColor.xy`, `cGrade.w`, `cWb.w` branch, `lin`, samplers s3..s5 and the whole HDR branch; renumber constants |
| `kAuxSource` (BrightPS, DownPS, UpPS, ControlPS) | drop | HDR glow and limiter |
| `Gpu` frame state, `CopyScene`, `OnGameDraw`, `OnFrameBoundary`, `RegisterHooks` | keep | the mask; keep `Priority::First`; `diagLog`/`diagSrgbWrite*` can go or become an SDR mask diagnostic |
| `BeforeOverlay` | keep the logic | PLANO-SEPARACAO.md 2d item 3 replaces it by the same test at the final pass |
| `InitResources` / `ReleaseResources` | keep frame, scene, chain, ps, queries | drop glow/up/meas/state targets |
| `OnEndScene` | keep the SDR path | drop `s_lampGain`, `HdrNative::Update`, `GetDisplayInfo`, `DepthShare::Request`, depth-stencil unbind, glow, limiter, `ReadSurface`/HDR_Diag (FP16 only) |
| `BeforeCreateDevice`, `CreateDeviceFailed`, `AfterCreateDevice`, `ApplyColorSpace`, `AfterReset`, `QueryDisplay`, `GetDisplayInfo` | drop | HDR device path; the early config read is not needed for SDR |
| `BeforeReset` | keep only `ReleaseResources` | no back buffer format change |
| `SaveToToml` / `LoadFromToml` | keep `[qol.picture]` write and read, keep reading grade keys from `[qol.hdr]` when `[qol.picture]` is missing | PLANO's one-time migration copies both tables from `S3SS.toml` into `ApexRadiance.toml`, so the fallback still matters |
| `RenderPictureUI` | keep | remove the "Running on the HDR image." branch; update the header tooltip in the Display tab |
| `RenderUI` (HDR section) | drop | also its line "Colour and image filters are in Picture" |
| `third_party/dxvk/d3d9_vk_ext.h`, `dxgi.lib`, `<dxgi1_6.h>` | drop | `d3dcompiler.lib` stays (runtime compile) |
| Fire points in `d3d9_hook.cpp` | re-home | PLANO 2d item 2: for SDR, running at Apex's own EndScene is correct in either hook order (Apex outer: S3SS menu drawn after the filters; Apex inner: the menu is masked as UI) |
| Log prefix "[HDR]", registry hook name "HdrOutput" | rename (suggested "[Picture]") | cosmetic |
| `lot_light_bridge.cpp` calls to `HdrOutput::LampGain()` | replace by 1 / remove | compile dependency of the removed HDR code |

## Open items
- An SDR diagnostic (dump the frame, the scene copy and the % of pixels treated as UI) to replace HDR_Diag for Picture.
- Verify whether in-game screenshots include the filters (the back buffer format is verified: A8R8G8B8).
- Verify behaviour with the game's own Edge Smoothing on.
- Measure the GPU cost at 1080p/1440p/4K (no figure recorded in the notes).
- Decide where the standalone runs the pass (Apex EndScene per PLANO 2d item 2) and keep the overlay-after-copy rule.

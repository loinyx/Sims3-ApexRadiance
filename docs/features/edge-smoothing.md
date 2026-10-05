# Edge Smoothing

Edge Smoothing removes the jagged, stair-stepped edges of roofs, fences, walls and furniture in the 3D world. It is
applied to the finished scene before the game draws any of its interface, so pie menus, tooltips, the HUD and the Apex
menu stay perfectly sharp. Two methods are offered: **FXAA** (recommended, lighter on the graphics card) and **SMAA 1x**
(cleaner long edges, sharper textures). It replaces the game's own Edge Smoothing option, which must be turned off.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (present in the first version in this repository) |
| Default | On for new configurations; method SMAA, quality High |
| Menu | System > Edge Smoothing; Overview > Image > Edge Smoothing |
| Configuration | `[patches.EdgeSmoothing]` in `ApexRadiance.toml` |
| Source | [`patches/edge_smoothing_patch.cpp`](../../patches/edge_smoothing_patch.cpp), [`third_party/smaa/`](../../third_party/smaa/) |

## The problem

The game's own "Edge Smoothing" (Options > Graphics) is hardware multisampling (MSAA). It is expensive at high
resolutions, and it makes the back buffer and depth buffer multisampled. Direct3D 9 cannot sample a multisampled depth
buffer, so every effect that reads the scene depth (Depth Blur, Ambient Occlusion, Water Reflections) stops working
while it is on. A post-process anti-aliasing injected on the final frame (as ReShade does) would also smooth, and soften,
the interface text.

## How Apex Radiance solves it

Apex Radiance runs a post-process anti-aliasing pass at the exact point where the game finishes its 3D scene and before
its first interface draw (the shared post-scene trigger described in [depth-blur.md](depth-blur.md#post-scene-trigger)).
The pass works only on the scene, coexists with the depth effects and needs no game code patch.

Each frame:

1. **Depth edges** (optional): convert the shared scene depth to log2 of the view distance, so object outlines can be
   found even where colours barely differ.
2. **Copy** the back buffer.
3. **Smooth:**
   - **FXAA:** one full-screen pass after FXAA 3.11 "quality": find edges from luma, search along them, blend across.
   - **SMAA 1x:** the unmodified reference `SMAA.hlsl` (Jimenez et al., MIT) in three passes: edge detection, blending
     weights from the precomputed area and search textures, neighbourhood blending.
4. **Sharpen** (optional): pixels the smoothing left alone get contrast-adaptive sharpening, so edges stay smooth while
   textures get crisper.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Edge Smoothing (card switch) | `enabled` | bool | on | | Turns the effect on |
| Method | `metodo` | enum | SMAA (1) | FXAA (0) *recommended*, SMAA (1) | FXAA is lighter and a little blurrier; SMAA keeps textures sharper |
| Quality (SMAA) | `qualidadeSmaa` | enum | High (2) | Low, Medium, High, Ultra, Extreme | Reference SMAA presets plus Extreme (table below). Shown with SMAA |
| Quality (FXAA) | `qualidade` | enum | Balanced (1) | Fast, Balanced, High, Extreme | Edge search length. Shown with FXAA |
| Softness (FXAA) | `suavidade` | float | 50% | 0 to 100% | Sub-pixel smoothing of thin lines; higher softens textures. Shown with FXAA |
| Sensitivity (FXAA) | `sensibilidade` | float | 77% (stored 0.125) | 0 to 100% (stored 0.333 to 0.063) | Edge contrast threshold; the menu shows `(0.333 - v) / (0.333 - 0.063)`, so higher catches fainter edges. Shown with FXAA |
| Edges from depth | `depthEdges` | bool | on | | Also smooths object outlines found in the scene depth; textures keep the normal threshold |
| Sharpen textures | `sharpen` | float | 0% | 0 to 100% | Contrast-adaptive sharpening of unsmoothed pixels. Independent of Color > Sharpness |
| Developer > Show smoothed pixels in red | `debugView` | bool | off | | Tints every pixel the smoothing changed 60% red (developer mode only) |

All settings apply live; a change only resets the GPU timer and retries
resources. Keys use the Portuguese names of the original build and must not be renamed. Settings removed from earlier
versions (`temporal`, `combineMsaa`) are no longer registered, so saved values cannot enable them.

The card shows a status line when there is an error, the GPU cost chip, a note when *Edges from depth* is on but the
depth is not available, and, at 1200 lines or fewer, the note *Smoothest at 1080p: NVIDIA DSR or AMD VSR with a higher
game resolution*. Driver supersampling is the only way to smooth detail thinner than a pixel.

**SMAA presets** (`kSmaaPresets`; the reference "SMAA Presets" with the threshold passed as a constant):

| Preset | `SMAA_THRESHOLD` | `SMAA_MAX_SEARCH_STEPS` | Diagonal steps | Corner rounding | Edge detection |
|---|---|---|---|---|---|
| Low | 0.15 | 4 | off | off | Luma |
| Medium | 0.10 | 8 | off | off | Luma |
| High (default) | 0.10 | 16 | 8 | 25 | Colour |
| Ultra | 0.05 | 32 | 16 | 25 | Colour |
| Extreme | 0.05 | 112 (reference maximum) | 20 (reference maximum) | 25 | Colour |

Colour edge detection (`SMAAColorEdgeDetectionPS`, macro `APEX_SMAA_COLOR_EDGES`) also finds edges between colours of
similar brightness, which luma detection misses. Extreme follows very long, nearly straight edges at 4K, where one step
can be longer than Ultra's reach (32 steps of 2 pixels per side).

**FXAA levels** (`kQualities`, macros `STEPS` / `STEP_SIZES`):

| Level | Steps | Step sizes (texels) |
|---|---|---|
| Fast | 5 | 1, 1.5, 2, 4, 12 (FXAA 3.11 preset 12) |
| Balanced (default) | 8 | 1, 1.5, 2, 2, 2, 2, 4, 8 |
| High | 12 | 1, 1.5, 2 x8, 4, 8 (preset 29) |
| Extreme | 16 | 1, 1, 1, 1, 1.5, 2 x6, 4, 4, 8, 8, 16 (57.5 texels each way, against 30.5 for High) |

## Compatibility and interactions

- **The game's Edge Smoothing (MSAA)** must be off. With a multisampled back buffer the effect pauses and releases its
  resources, and checks again every 120 frames (so turning the game option off is picked up after the device reset).
  Status: "The game's Edge Smoothing is on: turn it off in Options > Graphics to use this one".
- **Game anti-aliasing notice.** While the menu is open (after startup loading), it reads the real back-buffer
  multisample type. If it is multisampled and a conflicting effect is enabled (Edge Smoothing, Depth Blur, Ambient
  Occlusion, or Water Reflections with Night Lights on and a shore strength above zero), an amber card *Turn off the
  game's Edge Smoothing* lists the affected effects. It appears above the Overview, the Conflicts page and the page of
  each affected effect; lamp glow alone does not trigger it. Its single action, *How to turn it off*, expands the steps
  inline (Options > Graphics > Edge Smoothing Off). There is no popup, dismissal, stored preference or automatic change
  to game settings; Apex settings are kept. The condition is recomputed every time the menu draws, so the card persists
  across reopening and restarts while the conflict exists and disappears once the back buffer is single-sampled or the
  effects are off. A failed or unknown query shows no warning. The sidebar's Conflicts page appears only while a
  conflict exists, and affected Overview rows show *Waiting for game settings*. Texts are translated (EN/PT/ES/FR).
- **Post-scene order:** Ambient Occlusion (10), Edge Smoothing (20), Depth Blur (30). Depth Blur blurs the smoothed
  image.
- **Depth share:** with *Edges from depth* on (and the game's MSAA off), Edge Smoothing requests the INTZ depth swap, so
  the depth share runs even with Depth Blur off. The depth pass runs only when the shared depth is the depth-stencil
  bound at the trigger; otherwise the colour-only variants run.
- **Picture filters** copy the scene after the post-scene effects, so smoothed pixels are graded like the rest of the
  scene.
- **Hidden game UI:** when the game draws no depth-off interface after the scene, the post-scene chain runs at the end
  of the frame instead, so smoothing still applies.
- **Compare with the game** (shortcut) turns Edge Smoothing off with the other effects until it is pressed again.
- **Filtered screenshots:** the screenshot shortcut reads the back buffer after the post-scene effects and Picture; see
  [bug-reports.md](bug-reports.md#screenshot-settings).
- **Night Lighting pond pass** is marked as an internal pass, so its depth-off draw does not trigger the chain early.
- **Requirements:** `d3dcompiler_47.dll` (runtime-compiled shaders), R32F render targets for depth edges (optional).

## Limitations

- Detail thinner than a pixel (wires, thin rails, distant leaves) and shimmer in motion are not solved by any
  post-process anti-aliasing; driver supersampling is the remedy.
- The effect runs before the game's bloom composite, so bloom halos are not smoothed.
- SMAA reads and blends in gamma space (all samplers `SRGBTEXTURE = 0`). The reference allows this.
- Softness and Sensitivity apply to FXAA only; SMAA's threshold comes from the preset.
- In some interiors the game has depth-off back-buffer draws in the middle of the scene. The trigger fires at the first
  one, so geometry drawn after it is not smoothed (inferred from captures, not observed as a complaint).
- No temporal smoothing and no combination with the game's MSAA.

## Technical reference

**Lifetime**

1. `Install()` registers a Present hook (`"EdgeSmoothing"`, `Priority::First`), `preReset` / `postReset` callbacks and
   the effect at `PostScene::kEdgeSmoothing = 20`.
2. `OnFrameBoundary` (every Present): applies a pending settings change, holds or releases the depth request
   (`DepthShare::Request` while `depthEdges` is on and the game's MSAA is off), releases resources if MSAA appeared, and
   while not ready calls `InitResources` now and then every `kRetryFrames = 120` frames.
3. `InitResources`: checks `MultiSampleType == D3DMULTISAMPLE_NONE`; creates `copyTex` (back-buffer size and format; the
   game's back buffer is `D3DFMT_A8R8G8B8`), `edgesTex` and `blendTex` (back-buffer size, A8R8G8B8), the SMAA lookups,
   the optional R32F `depthLogTex`, and 4 sets of timestamp queries. Logs `[EdgeSmoothing] Resources ready (WxH)`.
4. **Reset:** `OnPreReset` releases every resource and query (status "Recreating after a video change..."); `OnPostReset`
   retries at the next Present. Shader objects survive.
5. `Uninstall()` removes the effect, drops the depth request, unregisters hooks, releases resources and shaders.

**Shaders.** All variants are compiled at start-up on a background thread (`framework/shader_cache.h`): 4 FXAA levels,
5 SMAA presets x 3 passes, 5 SMAA edge passes with predication, 4 FXAA depth variants and `LogDepthPS` (ps_3_0,
`D3DCOMPILE_OPTIMIZATION_LEVEL3`). The defaults (FXAA Balanced, SMAA High) compile first. The render thread only creates
shader objects from the kept bytecode on first use; a failure is logged once and not retried until reinstall.

**Per frame** (`FxaaEffect`, PostScene order 20): returns unless ready, not suspended and not MSAA. Timing key =
`method x 10 + quality`; a change resets the GPU-cost average. Timestamps are read without waiting (4 rotating sets,
`gpuMs = 0.9 gpuMs + 0.1 ms`). Then `RunDepthPass`, then `RunSmaa` or `RunFxaa`.

**Depth pass** (`RunDepthPass`): requires `depthEdges`, `depthLogTex`, the depth shaders and the shared depth bound as
the depth-stencil. Draws `LogDepthPS` into the R32F target: `1/z = (A - d) / (near A)`, output `-log2(max(A - d, 1e-7) /
(near A))`, sky (`d >= 0.99999`) = 16 (64 km). `c2 = (A, 1 / (near A))` with `near = PostScene::CameraNear()` (0.25 when
unknown) and `A = PostScene::CameraDepthA()`. The edge passes read it at `s5` (point).

- SMAA: reference predication, `SMAA_PREDICATION 1`, threshold 0.02 log2 (a 1.4% distance step), scale 1.0, strength
  0.6 (the threshold x 0.4 where the depth steps); textures keep the preset threshold.
- FXAA: its contrast test x 0.4 (`DEPTH_EDGE_SCALE`) where any of the 4 neighbours' log depth differs by more than 0.02
  (`DEPTH_STEP`).

**SMAA 1x** (`RunSmaa`):

1. `StretchRect(backbuffer -> copyTex, D3DTEXF_NONE)`. A failed copy sets status "ERROR: could not copy the scene for
   SMAA" and suspends the effect until a reset or settings change.
2. Detach the depth-stencil; save RT0, PS, VS, declaration / FVF, stream 0, textures and 8 sampler states of s0..s4,
   14 render states, PS constants c0..c2, viewport. No state block (CPU heavy).
3. Neutral state: no VS, FVF `XYZRHW | TEX1`, Z, blending, alpha test, stencil, scissor, fog, sRGB write and clip planes
   off, cull none, `MULTISAMPLEANTIALIAS` on with mask `0xFFFFFFFF`. Samplers s0..s4 linear and clamp, **s3 (searchTex) point**, no mips, `SRGBTEXTURE = 0`.
4. Constants: `c0 = (1/W, 1/H, W, H)` (`SMAA_RT_METRICS`), `c1 = (preset threshold, sharpen, 0, debug)`,
   `c2 = 0` (subsample indices: SMAA 1x).
5. Edge pass into `edgesTex` (cleared every frame, because the reference discards pixels with no edge); s0 = copy.
6. Weight pass into `blendTex` (cleared); s1 = edges, s2 = areaTex, s3 = searchTex.
7. Blend pass into the back buffer, RGB write only (the game's alpha is kept); s0 = copy, s4 = weights.
8. Restore RT0 first (it resets the viewport), then the depth-stencil, then the rest; stream 0 explicitly because
   `DrawPrimitiveUP` clears it.

**Wrapper.** `SMAA.hlsl` is embedded byte for byte (`third_party/smaa/smaa_hlsl.h`, `kSmaaHlsl`) between `kSmaaPrefix`
(`SMAA_RT_METRICS cMetrics`, `SMAA_HLSL_3`, `SMAA_THRESHOLD cParams.x`) and `kSmaaSuffix` (entry points `SmaaEdgePS`,
`SmaaWeightPS`, `SmaaBlendPS`). The reference's vertex-shader offset functions (`SMAAEdgeDetectionVS`,
`SMAABlendingWeightCalculationVS`, `SMAANeighborhoodBlendingVS`) are called at the top of each pixel shader because the
quads are pre-transformed. Source name "SMAA.hlsl".

**Lookup textures** (`CreateSmaaLookups`, `D3DPOOL_MANAGED`, one mip): `areaTex` 160 x 560 `D3DFMT_A8L8` from
`AreaTex.h` (R8G8 data; A8L8 stores L in the low byte, so the reference's R lands in `.r` and G in `.a`, read with
`SMAA_AREATEX_SELECT(sample) = sample.ra`), and `searchTex` 64 x 16 `D3DFMT_L8` from `SearchTex.h`. Any other format or
channel order feeds wrong area values; linear filtering of searchTex corrupts the packed search results.

**FXAA** (`RunFxaa`, `kShaderSource`): copy, save sampler 0, texture 0, c0..c2 and the touched states, s0 linear clamp
(FXAA reads between texels on purpose), draw one quad into the back buffer (RGB write), restore. Constants:
`c0.xy = 1/size`, `c1 = (softness, sensitivity, sensitivity / 3, debug)`, `c2.x = sharpen`. Luma is
`dot(rgb, (0.299, 0.587, 0.114))` in gamma space. Early exit when the 5-tap range is below
`max(sensitivity / 3, rangeMax x sensitivity)` (x 0.4 at a depth edge): about 5 texture reads for most of the screen.
Otherwise 4 diagonal taps, horizontal / vertical decision, sub-pixel amount `subpixH = smoothstep-like^2 x softness`,
edge-end search with the level's step sizes, one final bilinear read.

**Sharpening** (both methods; after AMD FidelityFX CAS, MIT): 4 neighbours,
`amp = sqrt(saturate(min(mn, 1 - mx) / mx))`, `w = -amp / lerp(8, 5, amount)`,
`c' = saturate((c + (n + s + w + e) w) / (1 + 4w))`. Applied only where SMAA computed no blend weight around the pixel
or FXAA took its early exit.

**Debug view.** FXAA tints every pixel that did not take the early exit; SMAA tints pixels with non-zero blend weights.
Developer mode only.

**Status strings.** "Waiting for the game...", "Active", "The game's Edge Smoothing is on: turn it off in Options >
Graphics to use this one", "ERROR: not enough video memory for the screen copy", "ERROR: not enough video memory for
SMAA", "ERROR: could not copy the scene for SMAA", "ERROR: SMAA did not compile (see ApexRadiance_LOG.txt)", "ERROR: the
shader did not compile (see ApexRadiance_LOG.txt)", "Recreating after a video change...", "Off". The Developer page
shows *Frames smoothed: N (with the scene depth M)* and, under *Rendering details*, the status and the GPU cost.

**Log lines.** `[EdgeSmoothing] Installed`, `[EdgeSmoothing] Resources ready (WxH)`, `[EdgeSmoothing] SMAA <entry>
(preset N) failed to compile: ...`, `[EdgeSmoothing] Shader (quality N) failed to compile: ...`,
`[EdgeSmoothing] No video memory for the depth-edge target: edges from colour only`.

**Game code.** None patched. The effect depends only on the post-scene draw pattern (at least 20 depth-tested back-buffer
draws, then a depth-off back-buffer draw or the end of the frame) and on a single-sampled back buffer.

**Cost.** One full-screen copy plus one (FXAA) or three (SMAA) full-screen passes, plus the optional depth pass. The
card shows the measured GPU cost; no per-preset figures are recorded.

## Rejected approaches

- Temporal SMAA (T2x) with jittered scene draws: removed; pool water artefacts and history-target cost.
- Combining the game's MSAA with SMAA: removed; it needs multisampled targets and breaks the depth effects.
- A full state block around the passes: CPU heavy; only touched states are saved.
- Reinstalling from `Update()` when settings change: settings are read live instead.

Details in [history](../history/edge-smoothing.md).

## See also

- [Validation](../validation/edge-smoothing.md)
- [History](../history/edge-smoothing.md)
- [Depth Blur: post-scene trigger and depth share](depth-blur.md)
- [Architecture: post-scene chain](../architecture.md)

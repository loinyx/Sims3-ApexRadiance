> Published 2.5.5 and retained in 2.5.6: FXAA is first and recommended; SMAA is spatial only. Temporal smoothing is removed. The feature defaults on for new configurations; existing choices are preserved. One unified build offers optional Developer mode in Settings. Older baseline details below are historical.

# Edge Smoothing (SMAA 1x / FXAA)

> Post-process anti-aliasing of the finished 3D scene, applied **before the game draws any UI**, so pie menus,
> tooltips, the HUD and both overlays stay sharp. Two methods: **SMAA 1x** (the unmodified reference `SMAA.hlsl` by
> Jimenez et al., MIT, in its `SMAA_HLSL_3` path; default) and a single-pass **FXAA 3.11-style "quality"** shader written
> for ps_3_0. By default it runs with the **game's own Edge Smoothing (MSAA) off**. Status: working, used in game by the user
> (log `[EdgeSmoothing] Resources ready (3840x2160)`, SMAA Ultra in the user's config), flagged `experimental`. Present
> in both build flavours; the Developer subsection is dev-only.

| Fact | Value |
|---|---|
| Patch name / TOML table | `EdgeSmoothing` / `[patches.EdgeSmoothing]` (combined `S3SS.toml`; standalone `ApexRadiance.toml`, same table name, PLANO-SEPARACAO.md "Config schema") |
| UI (combined build) | Display tab, `RenderApexFeature("EdgeSmoothing", "Edge Smoothing")` after Picture (`gui.cpp` approx. 620); display name "Edge Smoothing (SMAA / FXAA)" |
| Default | patch off until the user enables it (patch-system default, unverified for a fresh config); method SMAA, preset High |
| Build flavour | dev and public; "Developer" tree node compiled out of public (`kPublicBuild`) |
| Game code patched | none (pure D3D9 post-process) |
| Needs | game Options > Graphics > Edge Smoothing **off**; runtime `D3DCompile` (`d3dcompiler_47.dll`, linked through `d3dcompiler.lib`) |
| Credits | "Credits: @loinyx" at the end of the feature description; third-party credit "SMAA by Jimenez et al. (MIT)" in releases |

## Purpose

The game's own "Edge Smoothing" is multisampling. It blurs nothing, but it costs a lot at 4K and it makes the depth
buffer multisampled, which D3D9 cannot sample: Depth Blur, the pond scenery reflection and every other depth-reading
effect stop working with it on ([depth-blur.md](depth-blur.md), [reflections.md](reflections.md)). Edge Smoothing gives a
post-process replacement that runs on the scene only (a ReShade-style AA would also smooth, and soften, the UI text) and
coexists with the depth effects.

History (NOTAS-ILUMINACAO.md): FXAA added 26/09 together with the PostScene trigger ("Pos-processamento: FXAA + SSAO +
gatilho unico"); SMAA 1x added 28/09 ("SMAA 1x (28/09)"), made the default method.

**Standalone baseline.** The standalone takes Edge Smoothing (and Depth Blur) from the v0.1.0 commit `b84d5f1` ("Night
Remake alpha", FXAA only, `S3SS_TR` status strings) **plus** the SMAA 1x code and the Depth Blur map-view fade of
`combined-final`. Line references below are for `combined-final`, which already contains all of it.

## Settings

All settings are read live every frame (`Update()` only clears `pendingReinstall`). Keys are the Portuguese names kept
from the combined build.

| UI label | TOML key | Type | Default | Range / values | Effect |
|---|---|---|---|---|---|
| (feature toggle) | `enabled` | bool | false | | Install / uninstall the patch |
| Method | `metodo` | enum int | 1 | 0 FXAA, 1 SMAA | "SMAA: smoother long edges and sharp textures (3 passes). FXAA: lighter, a little blurrier." |
| Quality (shown when SMAA) | `qualidadeSmaa` | enum int | 2 | 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Extreme | The reference SMAA presets and Extreme (table below). "High and above also handle diagonals and corners; Ultra catches fainter edges (good at night); Extreme follows very long edges (4K) and colour edges." |
| Quality (shown when FXAA) | `qualidade` | enum int | 1 | 0 Fast, 1 Balanced, 2 High, 3 Extreme | FXAA edge-search length |
| Advanced > Softness | `suavidade` | float | 0.5 | 0 .. 1 | FXAA sub-pixel amount (`cParams.x`). Disabled (greyed) with SMAA |
| Advanced > Sensitivity | `sensibilidade` | float | 0.125 | 0.063 .. 0.333 | FXAA edge threshold (`cParams.y`); the minimum threshold is `sensitivity / 3` (`cParams.z`). Disabled with SMAA |
| Edges from depth (both methods, 30/09) | `depthEdges` | bool | true | | Requests the INTZ depth swap (DepthShare::Request) and, when the scene depth is the bound depth-stencil at the trigger (as AO checks), runs a pass writing log2 of the view distance (R32F, `LogDepthPS`: 1/z = (A - d)/(near A) from PostScene) that the edge passes read at s5 (point). SMAA: the reference predication (`SMAA_PREDICATION` 1, threshold 0.02 log2 = a 1.4% distance step, scale 1.0, strength 0.6 = the threshold x 0.4 where the depth steps; textures keep the preset threshold). FXAA: its contrast test x 0.4 where any of the 4 neighbours' log-depth differs by more than 0.02. Without the depth (swap off, reflection pass bound) the plain variants run; the card notes it |
| Sharpen textures (both methods, 30/09) | `sharpen` | float | 0 | 0 .. 1 | Contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT: 4 neighbours, weight -amp/lerp(8, 5, amount), amp = sqrt(min(mn, 1 - mx)/mx)) of the pixels the smoothing left as they were (SMAA: no blend weight around the pixel; FXAA: its early-out), so the smoothed edges are never sharpened again. 0 = off (no visual change). Independent of Color > Sharpness |
| Removed historical option: Temporal smoothing (30/09) | `temporal` | bool | false | | SMAA T2x. The scene draws (render target 0 = back buffer, depth test on) are drawn with vertex-shader copies moved in clip space by c252 (`ShaderPatches::AddJitterVs`, bound by the shared scene-draw binder in scene_dither.cpp, `SceneBinder`), alternating (+0.25, -0.25) and (-0.25, +0.25) pixels; SMAA runs with the matching subsample indices (1,1,1,0) / (2,2,2,0); then `ResolvePS` blends 50/50 with the previous frame's SMAA output fetched through M = VP_prev x inverse(VP_cur) (PostScene camera, double precision on the CPU) from the current device depth, clamped to the current 3x3 min/max and dropped where the previous log2 depth (2x2 around the point) differs by more than 0.03 log2 or the point left the screen; camera cut (screen centre moved > 0.3 NDC), no camera, no depth or no scene in a frame: no blend and no jitter. Needs the INTZ depth (requested). Offline: vs_3_0 3323/3323 and vs_2_0 4312/4338 game vertex shaders patched and accepted by a native device; one-pixel shift verified. Developer: blend view (green / magenta), jitter pairing swap, coverage counts |
| Developer > Show smoothed pixels in red (dev) | `debugView` | bool | false | | Tints every pixel the AA changed 60% red |

Status line on the card: `Status: <text>` and, once timings arrive, `GPU cost: x.xx ms per frame`. Developer also shows
`Frames smoothed: N`. Status strings: "Waiting for the game...", "Active", "The game's Edge Smoothing is on: turn it off in
Options > Graphics to use this one", "ERROR: not enough video memory for the screen copy", "ERROR: not enough video memory
for SMAA", "ERROR: SMAA did not compile (see S3SS_LOG.txt)", "ERROR: the shader did not compile (see S3SS_LOG.txt)",
"Recreating after a video change...", "Off".

### SMAA presets (`kSmaaPresets`, the "SMAA Presets" block of the reference, threshold passed as a constant)

| Preset | `SMAA_THRESHOLD` | `SMAA_MAX_SEARCH_STEPS` | Diagonal steps | Corner rounding |
|---|---|---|---|---|
| 0 Low | 0.15 | 4 | off (`SMAA_DISABLE_DIAG_DETECTION`) | off (`SMAA_DISABLE_CORNER_DETECTION`) |
| 1 Medium | 0.10 | 8 | off | off |
| 2 High (default) | 0.10 | 16 | 8 | 25 |
| 3 Ultra | 0.05 | 32 | 16 | 25 |
| 4 Extreme (30/09) | 0.05 | 112 (the reference's maximum) | 20 (its maximum) | 25; **colour** edge detection (`SMAAColorEdgeDetectionPS`, macro `APEX_SMAA_COLOR_EDGES`) |

**Extreme (30/09, user: "the anti-aliasing still does not leave things perfectly straight, even on Ultra, in both
modes"; built 31ab53c8, not tested in game yet).** At 4K one step of a nearly horizontal edge (a roof, a floor line) can
be longer than Ultra's reach (32 steps x 2 pixels per side), and SMAA then leaves it jagged; Extreme searches up to 112
steps and 20 diagonal steps, and detects edges on every colour channel (edges between colours of the same brightness,
which luma detection misses). FXAA got an Extreme level too (16 steps: 1, 1, 1, 1, 1.5, 2 x6, 4, 4, 8, 8, 16 = 57.5 texels
each way, against High's 30.5). Offline fxc (ps_3_0, /O3): SMAA edges 35 -> 43 slots (colour), weights 442 (the search is a
loop: unchanged), blend 62; FXAA Extreme 694 slots (High 538). Not solved by any post-process AA: detail thinner than a
pixel (wires, thin rails) and shimmer in motion; that needs supersampling (rendering at a higher resolution).

Each preset is a separate compile of the three entry points (macros per preset, `SmaaShaders`), compiled on first use
and cached for the session. **Standalone, 2026-09-28:** all 12 SMAA variants and the 3 FXAA qualities are compiled at
start-up on a background thread (`framework/shader_cache.h`, [architecture 4.6](../architecture.md#shader-precompile);
`kSmaaPsId`, `kFxaaPsId`); `SmaaShaders` / `ShaderFor` only create the shader objects from the kept bytecode, so the
first SMAA frame (the blending-weight pass was the slowest compile) and a quality change no longer compile on the render
thread. Same compile inputs and macros as before. Not tested in game yet.

### FXAA quality levels (`kQualities`, macros `STEPS` / `STEP_SIZES`)

| Level | Steps | Step sizes (texels) | Code comment |
|---|---|---|---|
| 0 Fast | 5 | 1.0, 1.5, 2.0, 4.0, 12.0 | FXAA 3.11 preset 12 |
| 1 Balanced (default) | 8 | 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 | "20-ish" |
| 2 High | 12 | 1.0, 1.5, 2.0 x8, 4.0, 8.0 | preset 29 |

## How it works

### Lifetime
1. `Install()` registers a registry **Present** hook (name `"EdgeSmoothing"`, `Priority::First`) that calls
   `OnFrameBoundary`, adds `OnPreReset` / `OnPostReset` to the Reset callbacks, and adds `FxaaEffect` to the PostScene
   chain at order `PostScene::kEdgeSmoothing = 20` (`patches/edge_smoothing_patch.cpp:682-699`).
2. `OnFrameBoundary` (every Present): while not `ready`, try `InitResources` immediately and then every
   `kRetryFrames = 120` frames (`:643-649`).
3. `InitResources` (`:342-379`):
   - reads the back-buffer description; if `MultiSampleType != D3DMULTISAMPLE_NONE` the game's MSAA is on: status message,
     no resources, retried every 120 frames (so turning the game's option off later picks it up after the device Reset);
   - `copyTex`: render-target texture of the back buffer's size **and format** (the game's back buffer is
     `D3DFMT_A8R8G8B8` = 21, verified in the log line `[HDR] Resources ready (3840x2160, format 21)` of 28/09);
   - SMAA targets `edgesTex` and `blendTex`: back-buffer size, `D3DFMT_A8R8G8B8`;
   - SMAA lookup textures (`CreateSmaaLookups`, `:308-319`, `D3DPOOL_MANAGED`, one mip):
     `areaTex` 160 x 560 **`D3DFMT_A8L8`** from `AreaTex.h` (R8G8 bytes; D3D9 A8L8 stores L in the low byte and A in the
     high byte, so the reference's R lands in `.r` and G in `.a`; the reference's `SMAA_HLSL_3` path reads it with
     `SMAA_AREATEX_SELECT(sample) = sample.ra`), and `searchTex` 64 x 16 **`D3DFMT_L8`** from `SearchTex.h`;
   - 4 sets of timestamp queries (`TIMESTAMPDISJOINT`, 2 x `TIMESTAMP`, `TIMESTAMPFREQ`);
   - log `[EdgeSmoothing] Resources ready (WxH)`, status "Active".
   Shaders are not compiled here: FXAA per quality level on first use (`ShaderFor`, `:321-340`), SMAA per preset on first
   use (`SmaaShaders`, `:274-304`). A compile failure is remembered (`compileTried` / `smaaTried`) and not retried until
   the patch is reinstalled.
4. **Reset** (alt-tab, resolution change): `OnPreReset` releases every default-pool resource and the queries
   (`ReleaseResources`, `:243-260`; the managed lookups too); `OnPostReset` sets `retryCountdown = 0`, so the next Present
   re-creates them. Compiled shaders survive.
5. `Uninstall()` removes the PostScene effect, unregisters the hooks and callbacks, releases resources and shaders.

### Per frame (the PostScene trigger)
PostScene (`post_scene.cpp`, see [depth-blur.md](depth-blur.md), "Shared machinery") fires once per frame at the first
back-buffer draw with `ZENABLE = FALSE` after at least 20 depth-tested back-buffer draws, which is the bloom composite
(when bloom is on) or the first UI draw. It calls the effects in order: Edge Smoothing (20), then Depth Blur (30). The
effects run inside the hooked game draw call, before that draw executes, so the AA sees the scene **without bloom and
without any UI**, and Depth Blur then blurs the already smoothed image.

`FxaaEffect` (`:616-641`):
1. Returns if not `ready`.
2. Timing key = `method x 10 + quality`; when it changes, the GPU-cost average and pending queries are reset.
3. `ReadTimings` (`:599-613`): collects any finished query set without waiting (`GetData` flags 0), converts ticks to ms,
   smooths `gpuMs = 0.9 gpuMs + 0.1 ms`. Issues a new begin on a free set (4 sets rotate, so results are read a few
   frames later).
4. `RunSmaa` or `RunFxaa`, then the end timestamps.

**SMAA 1x** (`RunSmaa`, `:487-596`):
1. `StretchRect(backbuffer -> copyTex, D3DTEXF_NONE)`: a plain copy of the scene.
2. Save exactly what the passes touch: RT0, PS, VS, vertex declaration / FVF, stream 0, textures and 8 sampler states of
   s0..s4, 12 render states (`kRenderStates`, `:386-388`), PS constants c0..c1, viewport. No state block (CPU heavy).
3. Neutral state: no VS, FVF `XYZRHW | TEX1`, Z off, blending off, alpha test off, stencil off, cull none, scissor off,
   fog off, sRGB write off, clip planes off. Samplers s0..s4 linear and clamp, **s3 (searchTex) point**, no mips,
   `SRGBTEXTURE = 0` everywhere.
4. Constants: c0 `SMAA_RT_METRICS` = (1/W, 1/H, W, H); c1 = (preset threshold, 0, 0, debug).
5. Pass 1, **luma edge detection**: RT = `edgesTex`, cleared to 0 (the reference `discard`s pixels with no edge, so the
   target must start empty every frame), s0 = scene copy, `SmaaEdgePS`.
6. Pass 2, **blending weights**: RT = `blendTex`, cleared, s1 = edges, s2 = areaTex, s3 = searchTex, `SmaaWeightPS`
   (`subsampleIndices = 0`: SMAA 1x, no temporal/MSAA modes).
7. Pass 3, **neighbourhood blending** into the back buffer: s0 = scene copy, s4 = blend weights, colour write RGB only
   (the back buffer's alpha stays the game's), `SmaaBlendPS`.
8. Restore (RT0 first because `SetRenderTarget` resets the viewport; stream 0 explicitly because `DrawPrimitiveUP`
   clears it).

All quads are `DrawPrimitiveUP` triangle strips with the -0.5 pixel offset of D3D9 (texel centres map to pixel
centres). `DrawPrimitiveUP` is not a registry hook, so the AA's own draws never re-trigger PostScene or count as game
draws for Picture.

**The wrapper around the reference** (`kSmaaPrefix` / `kSmaaSuffix`, `:151-190`): the reference `SMAA.hlsl` is embedded
byte for byte (`third_party/smaa/smaa_hlsl.h`, array `kSmaaHlsl`, generated from `SMAA.hlsl`) between a prefix that
defines `SMAA_RT_METRICS cMetrics`, `SMAA_HLSL_3` and `SMAA_THRESHOLD cParams.x`, and a suffix with three pixel shader
entry points. The reference computes its offsets in vertex shaders (`SMAAEdgeDetectionVS`,
`SMAABlendingWeightCalculationVS`, `SMAANeighborhoodBlendingVS`); here those functions are called at the top of each
pixel shader, because the effect draws pre-transformed quads with no vertex shader. Compiled `ps_3_0`,
`D3DCOMPILE_OPTIMIZATION_LEVEL3`, source name "SMAA.hlsl". Offline fxc check (NOTAS 28/09): 35 / 442 / 62 instruction
slots for the three passes (preset not recorded, unverified which).

**FXAA** (`RunFxaa`, `:395-478`, shader `kShaderSource`, `:43-133`):
1. `StretchRect(backbuffer -> copyTex)`, save the states touched (only sampler 0, texture 0, c0..c1), neutral state,
   s0 linear clamp (FXAA reads between texels on purpose), draw one quad with the render target left on the back buffer,
   restore.
2. Shader: luma = `dot(rgb, (0.299, 0.587, 0.114))` of each tap (gamma-space colour; no pre-computed luma in alpha).
   Early exit when the local range of the 5-tap cross is below `max(sensitivity / 3, rangeMax x sensitivity)` (5 texture
   reads for most of the screen, `[branch]`). Otherwise the FXAA 3.11 quality steps: 4 diagonal taps, horizontal /
   vertical edge decision, sub-pixel amount `subpixH = (smoothstep-like)^2 x softness`, edge-end search in both
   directions with the level's step sizes, offset along the gradient, one final bilinear read. Debug: lerp 60% to red
   on every pixel that did not take the early exit. SMAA's debug tints pixels whose blend weights (read like the
   reference's blending pass) are non-zero.
3. Offline fxc (NOTAS 26/09): 265 / 382 / 538 instruction slots for Fast / Balanced / High.

## Code map (combined tree, tag `combined-final`)

| File | Symbol | Lines | Role |
|---|---|---|---|
| `patches/edge_smoothing_patch.cpp` | `kShaderSource` (FXAA) | 43-133 | FXAA ps_3_0 source |
| | `kQualities` | 136-144 | FXAA step tables |
| | `kSmaaPrefix`, `kSmaaSuffix` | 151-190 | SMAA wrapper, three PS entries |
| | `kSmaaPresets`, `Params`, `AaState` | 193-232 | presets, settings, state |
| | `ReleaseResources`, `ReleaseShaders` | 243-271 | cleanup |
| | `SmaaShaders`, `CreateSmaaLookups`, `ShaderFor` | 274-340 | compile / lookups |
| | `InitResources` | 342-379 | targets, MSAA check |
| | `RunFxaa`, `DrawQuad`, `RunSmaa` | 395-596 | the passes |
| | `ReadTimings`, `FxaaEffect` | 599-641 | GPU cost, PostScene entry |
| | `OnFrameBoundary`, `OnPreReset`, `OnPostReset` | 643-660 | Present / Reset |
| | `EdgeSmoothingPatch` (settings, Install, Uninstall, UI), `APEX_REGISTER_FEATURE` | 664-787 | patch |
| `third_party/smaa/` | `SMAA.hlsl`, `smaa_hlsl.h`, `AreaTex.h` (160x560, pitch 320), `SearchTex.h` (64x16), `LICENSE.txt` | | reference code (MIT), unchanged |
| `post_scene.cpp/.h` | `PostScene::Add/Remove`, `kEdgeSmoothing = 20` | | trigger |

## Game addresses and patterns

None. Edge Smoothing depends only on the draw pattern the PostScene trigger relies on (>= 20 depth-tested back-buffer
draws, then the first depth-off back-buffer draw) and on the game's back buffer not being multisampled.

## Interactions

- **Game's Edge Smoothing (MSAA):** mutually exclusive. With MSAA on, the effect never becomes ready (status tells the
  user). The same applies to Depth Blur and the pond scenery reflection.
- **Depth Blur (order 30):** runs after the AA on the same trigger; it blurs the smoothed image. Edge Smoothing does not
  need the INTZ depth swap.
- **Ambient Occlusion (order 10, combined build only):** ran before the AA. Removed in the standalone
  ([../removed-features.md](../removed-features.md)).
- **Picture filters** ([picture-filters.md](picture-filters.md)): Picture separates scene from UI by comparing the final
  frame with a scene copy taken in its own `Priority::First` draw hooks at the depth-on -> depth-off transition, and marks
  as UI any pixel that differs by 1/64 or more. With bloom on, the copy is taken after the bloom strip, i.e. after the
  AA ran: fine. Without a bloom strip, Picture's copy and the PostScene trigger fire on the **same** draw, and the order
  of two `Priority::First` hooks is not defined (`std::sort`, not stable, `d3d9_hook_registry.cpp:79`). If the copy is
  taken first, the smoothed edge pixels differ from it and are treated as UI, so they stay ungraded (thin ungraded
  outlines when a strong grade is used). Inferred from the code, not observed. A fix for the standalone: give the two
  hooks explicit, different priorities, or let PostScene take the scene copy for Picture after the effects.
- **Night Lighting pond pass:** marked `DepthShare::SetInternalPass`, so its depth-off draw does not fire PostScene
  mid-frame.
- **HDR output (combined build only):** with HDR the back buffer was FP16, so `copyTex` was FP16 while `edgesTex` /
  `blendTex` stayed 8-bit. Irrelevant in the standalone.
- **S3SS overlay / Apex menu:** drawn later in EndScene, never smoothed.

## Limitations

- SMAA reads and blends in **gamma space** (all samplers `SRGBTEXTURE = 0`). The reference allows this ("If sRGB reads in
  this last pass are not possible, the technique will work anyway, but will perform antialiasing in gamma space",
  `SMAA.hlsl` note 5). Edge detection in gamma space is what the reference wants.
- SMAA 1x only: no temporal (T2x) or MSAA-combined (S2x/4x) modes. Luma edges (Extreme: colour edges); since 30/09 depth
  predication with "Edges from depth" (below).
- Runs before the game's bloom composite, so bloom halos themselves are not smoothed (irrelevant in practice).
- The trigger is the **first** depth-off back-buffer draw after 20 scene draws. In some interiors the game has depth-off
  draws in the middle of the scene (HDR diagnostic 28/09); geometry drawn after that point is not smoothed (inferred, not
  observed as a complaint; same caveat as Depth Blur).
- Softness and Sensitivity apply to FXAA only; SMAA's threshold comes from the preset.
- Cost scales with resolution (full-screen copy plus 1 or 3 full-screen passes). No measured GPU cost is recorded in the
  notes; read it from the card ("GPU cost").

## Pitfalls and failed approaches

- **AreaTex format:** the reference ships R8G8 data. D3D9 has no plain unsigned R8G8 texture format; the reference's
  own DX9 path uses A8L8 and reads `.ra`. Any other format or channel order feeds wrong area values to the blending
  weights. Keep A8L8 + `SMAA_HLSL_3` (no wrong variant was tried here; this is the reference's requirement).
- **SearchTex must be point-sampled** (s3); linear filtering corrupts the packed search results.
- **Clear edges and blend targets every frame:** the reference discards where there is nothing to do; stale content
  from the previous frame would blend ghost edges.
- **Do not write alpha** to the back buffer in the final pass (the game's alpha is kept; RGB write mask).
- **Save/restore precisely:** `SetRenderTarget` resets the viewport (restore RT first, viewport last),
  `DrawPrimitiveUP` clears stream 0 (restore it), and the game's next draw depends on c0..c1 and s0..s4.
- **Do not reinstall from `Update()`**; settings are live.
- FXAA is the older method (26/09); SMAA 1x was added on 28/09 and made the default. The UI hint states the trade-off:
  SMAA keeps textures sharper and smooths long edges better, FXAA is lighter and a little blurrier. No failed AA
  attempt is recorded in the notes.

## How to test

1. Game Options > Graphics > Edge Smoothing **off**. Enable Edge Smoothing in the Apex menu. Status must say "Active".
2. Log (`S3SS_LOG.txt`; standalone `ApexRadiance_LOG.txt`): `[EdgeSmoothing] Installed`, then `[EdgeSmoothing] Resources ready
   (WxH)`. Compile failures log `[EdgeSmoothing] SMAA <entry> (preset N) failed to compile: ...` or `[EdgeSmoothing]
   Shader (quality N) failed to compile: ...`.
3. Dev build: Developer > "Show smoothed pixels in red": edges of roofs, fences and furniture turn red; the UI must not.
   "Frames smoothed" increases every frame.
4. Compare presets on fences, roof edges and power lines; Ultra should also catch low-contrast edges at night.
5. Open a pie menu: its text must stay sharp (the AA runs before the UI).
6. Alt-tab / change resolution: "Recreating after a video change..." then "Active".
7. Turn the game's Edge Smoothing on: status must show the MSAA message and the image must be the game's MSAA only.
8. Frame Capture (dev, Ctrl+Shift+F9) lists the StretchRect and the DrawPrimitiveUP passes through the ExtraHooks
   observers ([dev-tools/frame-capture.md](dev-tools/frame-capture.md)).

## Open questions / unverified

- GPU cost per preset at 1080p / 1440p / 4K: not recorded.
- Which SMAA preset the offline slot counts (35 / 442 / 62) refer to.
- Whether the Picture copy / PostScene same-draw race happens in practice (frames without a bloom strip).
- Whether the game's own C screenshot contains the smoothed image remains unverified. Apex's optional player screenshot
  shortcut reads the back buffer at Present after Edge Smoothing and the other Apex passes; see
  [bug-reports.md](bug-reports.md#player-screenshots).

**Supersampling note (30/09, user: players at 1080p found the game very jagged with Edge Smoothing on).** At 1200 lines or fewer the card shows "Smoothest at 1080p: NVIDIA DSR or AMD VSR with a higher game resolution" (tooltip: where to turn it on; it costs more and the game's interface gets smaller). The driver renders at a higher resolution and scales down to the screen: the only thing that smooths detail thinner than a pixel. DLSS / DLAA / FSR 2+ are temporal (jittered camera, motion vectors) and have no D3D9 path; see NOTAS / the conversation of 30/09.


## Private RC test: temporal pool protection (2026-10-02)

Version `2.5.4-rc-temporal-pool-test` preserves the cumulative RC UI work. The user reports large triangles over pool water only when temporal SMAA is enabled and the camera rotates nearby. The 15:33:46 recording confirms temporal=true, method SMAA, Extreme, sharpen about 0.249. The supplied 2.88 s video shows geometric-looking strips rather than ordinary trails; point captures at 15:40:35 and 15:42:03 identify the same vs_2_0 pool shader. They also contain a screen-composition draw with depth enabled and ZFUNC=ALWAYS. The old crash file copied into these reports belongs to test006 on 2026-10-01; it is not evidence of a crash in this RC.

Candidate protection: AddJitterVs refuses the specifically identified pool family (compressed position decode constants 256/7.96875/-200 and 63.75/0/1 plus projected oT3.xy). Refusal leaves bytecode untouched; the existing unjittered game/grain path is retained. SceneBinder also excludes ZFUNC=ALWAYS composition draws from jitter. Identification occurs in shader-copy creation/caching, not by scanning shaders per draw. Other geometry retains temporal jitter. This avoids applying the main-scene jitter to that pool projection path; it is a targeted fallback, not proof of the underlying corruption mechanism. Unknown water variants are not guaranteed covered. Pool temporal edge quality may be reduced where it no longer receives jitter; the temporal resolve still runs globally.

Temporal quality: history reprojection now includes the actual current/previous quarter-pixel phases, while camera stillness remains based on unjittered camera matrices. History-copy failures invalidate reuse. The shader returns the current color for invalid/behind-camera/off-screen projections and caps slope tolerance at an additional 0.06 log2 units, so a depth discontinuity cannot make the rejection threshold unbounded. Existing 50/50 weight, SMAA presets, neighborhood clamp and stationary-camera behavior remain. No extra render pass, full-resolution target, readback or persisted setting was added. Shader/CPU cost and visual performance still require gameplay measurement.

Offline: captured pool original and grain fallback accepted by native D3D9, ordinary jitter retained, resolve ps_3_0 compiled and accepted, 108 phase/inverse/reprojection cases passed. These checks do not reproduce the pool artifacts. Test the same camera rotation/zoom near the pool with temporal on/off, then solid edges, moving Sims, shoreline/reflections and stationary camera. No installation or publication.

Primary references: [SMAA implementation](https://github.com/iryoku/smaa/blob/master/SMAA.hlsl) and [AMD temporal transparency guidance](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-temporal/). The latter discusses temporal failure on composited surfaces; this candidate does not implement FSR or an FSR reactive mask.


## RC decision: remove temporal smoothing (2026-10-02)

At the user's request SMAA T2x is removed from the Edge Smoothing feature. SMAA 1x and FXAA retain their existing spatial presets, depth edge detection and sharpening. The temporal setting and player/developer controls are no longer registered; legacy `temporal=true` in configuration or profiles cannot activate it. Configuration storage may retain unknown keys, but there is no bound temporal setting.

Removed: temporal resolve shader registration and implementation, history color/depth allocations, history copies, reprojection, camera requests and scene jitter acquisition/update. Previously the three full-resolution history targets were allocated even with the temporal option off. This removal avoids those allocations. Shared scene/shader helpers remain for reference; Edge Smoothing no longer calls them. The pool candidate above is historical and is superseded by this decision. No new AA algorithm, installation or publication is part of this change. Gameplay and performance remain to be validated.

Possible future spatial options: improve the existing SMAA presets/predication first; evaluate CMAA2 only after a D3D9 feasibility study (its reference implementation relies on a newer rendering API); combine native MSAA and SMAA only after supporting multisampled targets and the depth path explicitly. Supersampling is higher-cost and is not a low-FPS-cost replacement. No temporal technique is being introduced under a different name.


## Private RC: spatial SMAA only (2026-10-02)

High and Ultra use color edge detection to catch boundaries between different colors of similar brightness. Low/Medium retain luma detection; Extreme already used color. Thresholds, search lengths, diagonal handling and the three-pass pipeline are unchanged. Temporal smoothing remains removed.

The experimental native MSAA combination has been removed at the user's request. The game’s own Edge Smoothing must be off. Both SMAA and FXAA pause when a multisampled backbuffer is detected, preserving compatibility with Depth Blur and other depth-based effects. The old `combineMsaa` setting is no longer registered or read; retained configuration keys cannot enable it.

SMAA retains depth-stencil detach/restore around intermediate targets, saved render states and the screen-copy failure guard. No new pass, temporal history or CPU readback is introduced.

Offline checks cover five spatial presets, flat/diagonal fields, alpha and state restoration, equal-brightness color detection, and rejection of native MSAA targets. Gameplay and FPS still require validation. This is a private candidate, not installed or published.

FXAA is now labeled recommended in EN/PT/ES/FR for its lower GPU cost. This is a recommendation label change, not a forced migration of saved settings or a change to the registered default. SMAA remains selectable.

# Edge Smoothing: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/edge-smoothing.md](../features/edge-smoothing.md).

### 2026-09-26: FXAA and the post-scene trigger

**Context:** combined build. FXAA was added together with the single post-scene trigger (raw notes
"Pos-processamento: FXAA + SSAO + gatilho unico").

**Finding:** running inside the game's draw call, between the last scene draw and the first UI draw, smooths the scene
without the UI.

**Outcome:** kept. FXAA levels Fast, Balanced and High.

### 2026-09-28: SMAA 1x

**Context:** raw notes "SMAA 1x (28/09)". The reference `SMAA.hlsl` in its `SMAA_HLSL_3` path was embedded unchanged.

**Finding:** SMAA keeps textures sharper and smooths long edges better; FXAA is lighter and a little blurrier. The
AreaTex R8G8 data needs A8L8 read as `.ra` (the reference's DX9 layout); SearchTex must be point-sampled; the edge and
blend targets must be cleared every frame. No failed AA attempt was recorded.

**Outcome:** SMAA 1x became the default method (preset High). The combined build placed the card on the Display tab
after Picture (`RenderApexFeature("EdgeSmoothing", "Edge Smoothing")`, `gui.cpp`).

### 2026-09-28: combined-build interactions

**Context:** review of the combined build (`combined-final`, commit 45e36e2; code map `patches/edge_smoothing_patch.cpp`
lines 43-787 at that tag).

**Finding:** with HDR output on, the back buffer was FP16, so `copyTex` was FP16 while `edgesTex` / `blendTex` stayed
8-bit. Picture's scene copy and the post-scene trigger were both `Priority::First` draw hooks; without a bloom strip they
could fire on the same draw in an undefined order (`std::sort`, not stable), and if the copy ran first the smoothed edge
pixels would differ from it by more than 1/64 and be treated as UI (left ungraded). Inferred from the code, never
observed.

**Outcome:** HDR output was removed from the standalone. Picture's draw hooks now run at priority 10, after the
post-scene hooks (`Priority::First`), so the copy always contains the smoothed image.

### 2026-09-28: standalone baseline and precompile

**Context:** the standalone took Edge Smoothing (and Depth Blur) from v0.1.0 commit `b84d5f1` ("Night Remake alpha",
FXAA only, `S3SS_TR` status strings) plus the SMAA code and the Depth Blur map-view fade of `combined-final`.

**Finding:** the SMAA blending-weight pass was the slowest compile and stalled the render thread on first use.

**Outcome:** all SMAA and FXAA variants compile at start-up on a background thread (`framework/shader_cache.h`); the
render thread only creates shader objects.

### 2026-09-30: edges from depth and texture sharpening

**Context:** commit 0c54dc3. Faint edges (walls against walls of the same colour, night scenes) were not smoothed.

**Finding:** log2 of the view distance turns a depth step into a relative distance, the same near and far. SMAA has a
reference predication path; FXAA can scale its contrast test the same way.

**Outcome:** *Edges from depth* (on) and *Sharpen textures* (0%) added for both methods.

### 2026-09-30: Extreme presets

**Context:** maintainer feedback: even at Ultra, long edges were not perfectly straight with either method (build
31ab53c8).

**Finding:** at 4K one step of a nearly horizontal edge can be longer than Ultra's reach (32 steps x 2 pixels per side).
Extreme raises SMAA to 112 search steps and 20 diagonal steps with colour edge detection; FXAA Extreme uses 16 steps
(57.5 texels each way, against 30.5 for High). No post-process AA solves sub-pixel detail or shimmer in motion; that
needs supersampling.

**Outcome:** Extreme added to both methods.

### 2026-09-30: supersampling note at 1080p

**Context:** maintainer feedback: players at 1080p still found the game very jagged with Edge Smoothing on.

**Finding:** only rendering at a higher resolution smooths detail thinner than a pixel. DLSS, DLAA and FSR 2+ are
temporal (jittered camera, motion vectors) and have no D3D9 path.

**Outcome:** at 1200 lines or fewer the card suggests NVIDIA DSR or AMD VSR, with a tooltip on where to enable it.

### 2026-09-30: temporal smoothing (SMAA T2x)

**Context:** a *Temporal smoothing* option (`temporal`, off by default) was added.

**Finding:** design: scene draws (render target 0 = back buffer, depth test on) were drawn with vertex-shader copies
moved in clip space by `c252` (`ShaderPatches::AddJitterVs`, bound by the shared scene-draw binder in
`scene_dither.cpp`), alternating (+0.25, -0.25) and (-0.25, +0.25) pixels; SMAA ran with subsample indices (1,1,1,0) /
(2,2,2,0); `ResolvePS` blended 50/50 with the previous frame's output fetched through `M = VP_prev x inverse(VP_cur)`
(post-scene camera, double precision on the CPU) from the current depth, clamped to the current 3x3 min/max, dropped
where the previous log2 depth (2x2 around the point) differed by more than 0.03 log2 or the point left the screen.
Camera cuts (screen centre moved more than 0.3 NDC), no camera, no depth or no scene disabled blend and jitter. Offline:
vs_3_0 3323/3323 and vs_2_0 4312/4338 game vertex shaders patched and accepted; one-pixel shift verified. Developer
tools: blend view (green / magenta), jitter pairing swap, coverage counts.

**Outcome:** shipped as an option; later removed (below).

### 2026-10-02: temporal pool protection candidate

**Context:** private build `2.5.4-rc-temporal-pool-test`. Large triangles appeared over pool water only with temporal
SMAA on and the camera rotating nearby (recording 15:33:46: temporal on, SMAA Extreme, sharpen about 0.249; a 2.88 s
video showed geometric strips rather than trails). Point captures (15:40:35, 15:42:03) identified one vs_2_0 pool
shader and a screen-composition draw with depth on and `ZFUNC = ALWAYS`. A crash file in the reports belonged to an
older test of 2026-10-01 and was unrelated.

**Finding:** candidate fix: `AddJitterVs` refused the identified pool family (compressed position decode constants
256 / 7.96875 / -200 and 63.75 / 0 / 1 plus projected `oT3.xy`); the binder excluded `ZFUNC = ALWAYS` draws from jitter.
History reprojection included the actual quarter-pixel phases; history-copy failures invalidated reuse; invalid
projections returned the current colour; slope tolerance was capped at an extra 0.06 log2. Offline checks passed
(validation page) but did not reproduce the artefact. References: the SMAA implementation and AMD's temporal
transparency guidance (FSR reactive masks were not implemented).

**Outcome:** superseded by the removal of temporal smoothing.

### 2026-10-02: temporal smoothing removed

**Context:** decision for 2.5.5 to keep Edge Smoothing spatial only.

**Finding:** the three full-resolution history targets were allocated even with the option off.

**Outcome:** removed the resolve shader, history colour and depth targets, history copies, reprojection, camera
requests and scene-jitter acquisition. The `temporal` setting and its controls are no longer registered, so a saved
`temporal = true` cannot activate it. The shared scene-jitter helpers in `scene_dither.cpp` and `shader_patches.cpp`
remain but are not called. Possible future spatial work: improve the SMAA presets and predication first; evaluate CMAA2
only after a D3D9 feasibility study (its reference needs a newer API); combine MSAA and SMAA only after supporting
multisampled targets and the depth path explicitly. Supersampling is not a low-cost replacement.

### 2026-10-02: spatial SMAA with colour edges; MSAA combination removed

**Context:** private 2.5.5 candidate.

**Finding:** colour edge detection catches boundaries between different colours of similar brightness.

**Outcome:** High and Ultra use colour edge detection (Low and Medium keep luma; Extreme already used colour).
Thresholds, search lengths and the three-pass pipeline are unchanged. The experimental native MSAA combination
(`combineMsaa`) was removed; it is no longer registered or read. SMAA keeps the depth-stencil detach and restore, the
saved render states and the screen-copy failure guard.

### 2026-10-02: FXAA labelled recommended

**Context:** release 2.5.5.

**Finding:** FXAA has the lower GPU cost.

**Outcome:** FXAA is listed first and labelled *recommended* in all four languages. The registered default stays SMAA
High and saved choices are not migrated. New configurations have Edge Smoothing on.

### 2026-10-02: game anti-aliasing compatibility notice

**Context:** three compact pause notices on separate pages.

**Finding:** one shared, recomputed notice is clearer.

**Outcome:** replaced by the amber *Turn off the game's Edge Smoothing* card with inline instructions, gated by the
menu's loading state. See the feature page.

### 2026-10-03: restore-default buttons removed

**Context:** commit 1bb2291 removed page and card restore-default controls across the menu.

**Finding:** per-row reset (changed dots) remains.

**Outcome:** the *Reset Edge Smoothing* button (`g.p = Params{}`, every field including `debugView`) is gone.

### 2026-10-03: post-scene effects with hidden game UI

**Context:** commit abd4603. With the game UI hidden (F10) there may be no depth-off draw after the scene.

**Finding:** the post-scene chain never fired in those frames.

**Outcome:** the chain also runs at the game's EndScene, before Picture's scene copy and the Apex overlay, when no
qualifying boundary occurred. A boundary with a mismatched depth-stencil is rejected without consuming the effects.

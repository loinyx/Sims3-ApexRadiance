# Code map: Image effects and the post-scene chain

This area covers the effects that work on the finished 3D scene before the game draws its bloom and UI: the shared
post-scene trigger and INTZ depth, Ambient Occlusion with Sim Occlusion, Edge Smoothing, Depth Blur, the Picture
(Color) pass with LUTs, the Banding Fix with the High-precision scene, the water shore reflection and the Frame Capture
developer tool. Player-facing behaviour is on the linked feature pages; this page lists where the code is and what
must not break. [Code map](../code-map.md).

D3D9 hook priorities are `framework/d3d9_hooks.h:Priority` (First 0, Early 25, Normal 50, Late 75, Last 100; any int
is allowed). Everything below runs on the render thread unless stated.

## Frame order

| step | what runs | priority / order |
|---|---|---|
| Present (end of the previous frame) | `framework/scene_target.cpp:ResolveAtPresent` resolves a still-live 16-bit scene | Present -3000 |
| | ApexCore `framework/d3d9_bootstrap.cpp:OnPresent` | Present -2000 |
| | frame boundaries of PostScene, SceneDither, AO, Edge Smoothing, Depth Blur (INTZ init and retry), Picture | Present First, registration order |
| | Night Lighting pushes water settings (`features/lot_light_bridge.cpp:SetWaterFix`); Frame Capture | Present Last |
| | `framework/scene_target.cpp:ArmAtPresent`: the next frame starts in the 16-bit target | Present 100000 |
| Scene draws | depth-stencil swapped to INTZ (`patches/depth_blur_patch.cpp:SubstituteDS`), RT0 back buffer swapped to the 16-bit target (`framework/scene_target.cpp:SubstituteRt`) | detours |
| | PostScene `features/post_scene.cpp:OnGameDraw` counts scene draws and votes the camera | DIP/DP First |
| | Picture `features/picture.cpp:OnGameDraw` (fallback scene copy) | DIP/DP 10 |
| | AO `patches/ambient_occlusion_patch.cpp:RecordSimReceiver` (Sim mask draw before the game's) | DIP/DP Early |
| | `features/lot_light_bridge.cpp:DrawLake` (water glow and shore reflection) | DIP/DP Normal |
| | `features/scene_dither.cpp:OnDraw` (grain copies; skipped while the 16-bit target substitutes) | DIP/DP 1000 |
| Scene boundary | first back-buffer draw with ZENABLE off after at least 4 depth-tested draws, shared INTZ bound: `features/post_scene.cpp:RunEffects` | `PostScene::Order` |
| | fallback, hidden UI: the game's first 256x256 POINT tile copy into a learned 2048x1024 target, `features/post_scene.cpp:BeforeColourTile` | StretchRect detour |
| | fallback, no depth-off draw: `features/post_scene.cpp:AtEndSceneBeforeOverlay` | endSceneBeforeOverlay |
| Game EndScene | endSceneBeforeOverlay callbacks, `features/picture.cpp:Picture::BeforeOverlay`, overlay, `features/picture.cpp:Picture::OnEndScene` (end-of-frame fallback) | `framework/d3d9_bootstrap.cpp:Hooked_EndScene` |

### Post-scene passes (once per frame, ascending order)

| order | effect | runs without shared depth | reads | writes |
|---|---|---|---|---|
| 0 | `features/bloom_alpha_probe.cpp:CaptureAtSceneEnd` (developer, after a request) | no | back-buffer alpha via read-back | PNG and text report on a detached thread |
| 5 | `framework/scene_target.cpp:AtBoundary` (High-precision scene resolve) | yes | 16-bit target | back buffer RGBA, alpha passed through |
| 10 | `patches/ambient_occlusion_patch.cpp:AoEffect` | no | shared scene copy, INTZ, Sim masks | back buffer RGB |
| 20 | `patches/edge_smoothing_patch.cpp:FxaaEffect` | no | shared scene copy, INTZ (depth edges only) | back buffer RGB |
| 30 | `patches/depth_blur_patch.cpp:BlurEffect` | no | shared scene copy, INTZ | back buffer RGB, alpha-blended |
| 40 | `features/picture.cpp:BoundaryEffect` | yes | shared scene copy, reduced chain, depth (Emphasize), LUT | back buffer RGB |

Orders come from `features/post_scene.h:Order` and `framework/scene_target.cpp:kBoundaryOrder`. Every pass copies the
back buffer into the one shared texture (`framework/scene_copy.cpp:Acquire`) right before it reads it.

## Post-scene chain and INTZ depth share

Finds the end of the 3D scene once per frame and runs the registered effects there in fixed order; also reads the
camera and owns the shared scene copy. Docs: [Depth Blur](../features/depth-blur.md),
[Picture filters](../features/picture-filters.md), [camera](../engine/camera-and-map-view.md),
[shaders](../engine/shaders.md).

- **Files:** `features/post_scene.cpp` (trigger, effect list, camera vote), `features/depth_share.h` (API, implemented
  in `patches/depth_blur_patch.cpp`), `framework/scene_copy.cpp` (one shared full-resolution copy),
  `framework/d3d9_extra_hooks.cpp` (StretchRect, Set/GetDepthStencilSurface, Clear, DrawPrimitiveUP,
  DrawIndexedPrimitiveUP detours; target hooks GetRenderTargetData, ColorFill, GetRenderTarget).
- **Entry points:** `features/post_scene.cpp:PostScene::Add` / `PostScene::Remove` (effects' Install/Uninstall, through
  `SyncHooks`); `features/post_scene.cpp:OnGameDraw` (DIP/DP First); `features/post_scene.cpp:OnFrameBoundary` (Present
  First); `features/post_scene.cpp:BeforeColourTile` (`ExtraHooks::SetBeforeStretchRect`);
  `features/post_scene.cpp:AtEndSceneBeforeOverlay`; `features/post_scene.cpp:EffectsPending` (Picture);
  `patches/depth_blur_patch.cpp:DepthShare::Request` (AO Install, Edge Smoothing frame boundary, Picture
  `features/picture.cpp:RequestDepth`).
- **Game hooks and addresses:** device vtable slots 32, 34, 35, 38, 39, 40, 43, 83, 84 in
  `framework/d3d9_extra_hooks.cpp:EnsureInstalled` and `EnsureTargetHooks` (refused when a slot shares code with an
  owned one). Camera from vertex constants c40..c43; near plane voted over blocks c0, c4, c40, c180, c192, c216
  (`features/post_scene.cpp:VoteCamera`, `NearFromBlock`).
- **Threads:** render for hooks and effects; `PostScene::Add`/`Remove`/`WantCamera` lock `g_mutex` and run on the
  Install/Uninstall caller's thread (UNVERIFIED which).
- **State:** `features/post_scene.cpp:g_effects` (Add/Remove, read by `RunEffects`); `g_done`, `g_rejectedBoundary`,
  `g_uiDrawSeen` (boundary flags); `g_lastDepthWrites` (read by Depth Blur's frozen-screen gate); `g_knownTileTarget`
  (hidden-UI fallback); `g_near`, `g_vp` (camera for AO, Edge Smoothing, Picture); `patches/depth_blur_patch.cpp:g`
  (`requests`, `ready`, `internalPass`) behind `DepthShare`.
- **Settings:** none.
- **Tests:** `tools/post_scene_test/` runs the production `features/post_scene.cpp` (order, fallbacks, depth rejection,
  tile recognition) and `EnsureSceneCopy` extracted from `features/picture.cpp`; run `tools/post_scene_test/run.ps1
  -OutDir <dir>` (needs GPU).
- **Invariants:** effects draw only with DrawPrimitiveUP, which is not in the hook registry, so they never re-trigger
  the boundary (`features/post_scene.h`); each effect saves and restores only what it touches; never run over UI
  already drawn (`AtEndSceneBeforeOverlay`); depth on with ZFUNC ALWAYS and no depth write is UI, not scene
  (`OnGameDraw`); nothing runs before `ShaderCache::PrecompileComplete`; an effect that throws is disabled for the
  session (`RunEffects`); the lake pass wraps its depth-off draw in `DepthShare::SetInternalPass`.
- **Rejected:** counting the UI glass passes as scene; 20 then 8 minimum scene draws (now 4) ->
  [history](../history/depth-blur.md).
- **Fragile:** `kMinSceneDraws` in `features/post_scene.cpp` and `features/picture.cpp`; the "ALWAYS without depth
  write" rule in `features/post_scene.cpp:OnGameDraw`, `features/picture.cpp:OnGameDraw` and
  `features/scene_dither.cpp:OnDraw`; back-buffer and RT0 identity cached separately in PostScene, Picture
  (`gpu`), SceneDither (`g_backBuffer`), Depth Blur and SceneTarget (`g_bb`), each cleared at preReset; tile magic
  (256x256, 2048x1024, POINT) in `features/post_scene.cpp:FirstColourTile`; `SyncHooks` writes render-thread globals
  from the caller's thread; `DepthShare::Request` counts without a lock and `patches/depth_blur_patch.cpp:StopDepth`
  releases D3D resources on the caller's thread.

## Ambient Occlusion and Sim Occlusion

Deterministic full-resolution GTAO shade where surfaces meet; Sim Occlusion lowers or caps it on recognised Sim and
hair materials. Docs: [feature](../features/ambient-occlusion.md), [Sim Occlusion](../features/sim-occlusion.md),
[validation](../validation/ambient-occlusion.md), [history](../history/ambient-occlusion.md),
[Sim history](../history/sim-occlusion.md).

- **Files:** `patches/ambient_occlusion_patch.cpp` (passes, composite, Sim mask, settings, both cards),
  `features/sim_occlusion.h` (card declaration), `shaders/sim_receiver_ids.h` (generated Sim shader fingerprints),
  `features/shader_patches.cpp:MakeAoReceiverMask` (mask shader copies).
- **Entry points:** `patches/ambient_occlusion_patch.cpp:AoEffect` (post-scene 10) -> `RunAo`; `OnFrameBoundary`
  (Present First: Sim mask reset, `InitResources` retry); `RecordSimReceiver`, `IsSimReceiver`, `SimCopy` (DIP/DP and
  SetPixelShader Early); `SimOcclusion::RenderUI` (menu); `CaptureFrame` (developer depth capture).
- **Game hooks and addresses:** none; Sim receivers matched by bytecode size and hash against
  `shaders/sim_receiver_ids.h:kSimReceiverPs` and `kSimHairReceiverPs`.
- **Threads:** render; Sim hooks ignore any thread other than `simRenderThread`.
- **State:** `patches/ambient_occlusion_patch.cpp:g` (resources and `Params`); `simMask` (G32R32F masks, shader
  classifications with AddRef against address reuse); `simRenderThread`.
- **Settings:** *Strength* `[patches.AmbientOcclusion] forca` = 1.68; *Reach* `alcance` = 1.3; *Keep lamp light*
  `protegerLuz` = 0.38; *Quality* `qualidade` = 2 (High; stored 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Very Low);
  *Also in map view* `noMapa` = true; *Distance* `distance` = 351; *Sim Occlusion* switch `simControls` = true;
  *Sim intensity* `simStrength` = 0.40; *Hair intensity* `hairStrength` = 0.0; *Maximum darkening* `simMaxShade` =
  0.11; *Transparent hair* `transparentHair` = true; revision key `revisao` (`kSettingsRevision`).
- **Tests:** `tools/ao_receiver_test/` covers `MakeAoReceiverMask` over all recognised pairs and the composite HLSL
  (needs precomp and GPU); `tools/ao_receiver_test/run_runtime_checks.ps1` replays `RecordSimReceiver` (needs GPU).
- **Invariants:** no temporal accumulation or per-frame noise; runs only in world with the INTZ bound as the current
  depth-stencil (`AoEffect`); INTZ unbound before sampling (`SetPassStates`); composite writes RGB only (alpha is the
  bloom mask); Sim mask uses MIN blend and only LESS/LESSEQUAL draws; `kGtaoPsId` follows `kQualitySlices`
  (static_assert); settings are read live and never reinstall; new defaults need a `kSettingsRevision` bump.
- **Rejected:** half resolution, temporal noise, near-unit radii, plain multiply composite; temporal smoothing and
  thin-object detail -> [history](../history/ambient-occlusion.md), [removed features](../removed-features.md).
- **Fragile:** `SavedState`, `DrawQuad`, `QuadVertex` duplicated with `patches/depth_blur_patch.cpp`;
  `run_runtime_checks.ps1` extracts the source between `struct SimMaskCopy {` and `void ReleaseResources() {`; the
  composite grain follows `SceneDither::Strength`; `simMask.known` stops classifying after 2048 shaders.

## Edge Smoothing (SMAA/FXAA/temporal)

Spatial SMAA 1x or FXAA 3.11 on the scene before the UI, with optional depth edges and texture sharpening; the temporal
mode is removed. Docs: [feature](../features/edge-smoothing.md), [validation](../validation/edge-smoothing.md),
[history](../history/edge-smoothing.md).

- **Files:** `patches/edge_smoothing_patch.cpp` (shaders, depth-log pass, effect, settings), `third_party/smaa/`
  (reference SMAA), `features/temporal_sampling.h` (only used by `tools/temporal_check/`).
- **Entry points:** `patches/edge_smoothing_patch.cpp:FxaaEffect` (post-scene 20) -> `RunDepthPass`, `RunSmaa` or
  `RunFxaa`; `OnFrameBoundary` (Present First: depth request, `InitResources` retry).
- **Game hooks and addresses:** none; the game's own MSAA is read from the back buffer's `MultiSampleType`
  (`InitResources`) and pauses the effect.
- **Threads:** render; `g_settingsChanged` is set by `Update` on the patch manager's thread (UNVERIFIED which).
- **State:** `patches/edge_smoothing_patch.cpp:g` (`gameAaOn`, `depthRequested`, targets); `g_settingsChanged`.
- **Settings:** *Method* `[patches.EdgeSmoothing] metodo` = 1 (SMAA); *Quality* (SMAA) `qualidadeSmaa` = 2 (High);
  *Quality* (FXAA) `qualidade` = 1 (Balanced); *Softness* `suavidade` = 0.5; *Sensitivity* `sensibilidade` = 0.125
  (shown inverted); *Edges from depth* `depthEdges` = true; *Sharpen textures* `sharpen` = 0; developer *Show smoothed
  pixels in red* `debugView` = false.
- **Tests:** `tools/spatial_aa_check/` (`prepare.py` cuts the source before `class EdgeSmoothingPatch`; build line
  UNVERIFIED; needs GPU). `tools/temporal_check/` is stale: it expects `kResolveSource` and `Invert`, which no longer
  exist.
- **Invariants:** paused while the game's MSAA is on; final pass writes RGB only; depth is requested only while
  *Edges from depth* is on; runs only in world.
- **Rejected:** temporal SMAA T2x (history targets allocated even when off; pool artefacts); CMAA2, MSAA plus SMAA,
  supersampling -> [history](../history/edge-smoothing.md).
- **Fragile:** `RunDepthPass` samples the INTZ while it is still bound as depth-stencil (only ZENABLE off), unlike AO,
  Depth Blur and Picture (hazard UNVERIFIED); dead temporal code remains in `features/scene_dither.cpp:SceneBinder`,
  `features/shader_patches.cpp:AddJitterVs` and `features/temporal_sampling.h`.

## Depth Blur

Depth-of-field blur behind an Auto or Fixed focus; the same module owns the INTZ depth swap every depth effect reads.
Docs: [feature](../features/depth-blur.md), [validation](../validation/depth-blur.md),
[history](../history/depth-blur.md), [camera](../engine/camera-and-map-view.md).

- **Files:** `patches/depth_blur_patch.cpp` (swap, `DepthShare`, focus/prep/blur/composite, settings),
  `features/depth_share.h`.
- **Entry points:** `patches/depth_blur_patch.cpp:BlurEffect` (post-scene 30) -> `RunBlur`; `InitResources` (from
  `OnFrameBoundary`, Present First, registered by `StartDepth`); `StartDepth` / `StopDepth` / `UpdateDepth` (Install,
  Uninstall, `DepthShare::Request`); `WorldPlayable`, `NightSettled`, `NightLive` (world gate; `NightLive` is installed
  into `WorldSession::g_loadSettled` by a static initializer).
- **Game hooks and addresses:** SetDepthStencilSurface and GetDepthStencilSurface substitution through
  `framework/d3d9_extra_hooks.cpp:SetDepthSubstitution`; only a single-sample D24S8/D24X8 depth the size of the back
  buffer is swapped.
- **Threads:** render for passes; `DepthShare::Request` and `StopDepth` on the caller's thread.
- **State:** `patches/depth_blur_patch.cpp:g` (`origDS`, `intzTex`, `intzSurf`, `blurOn`, `requests`, `active`,
  `world`, `mapFade`, `worldIn`).
- **Settings:** *Focus* `[patches.DepthBlur] focoAuto` = false (Fixed); *Blur amount* `quantidade` = 1.0; *Sharp area*
  `areaNitida` = 2 (Large); *Focus speed* `velocidadeFoco` = 0.1; *Glowing lights* `realceLuzes` = true; *Distance*
  `distancia` = 0.318108052; *Transition* `transicao` = 0.287828237; *Strength* (Advanced) `forca` = 1.0; *Quality*
  `qualidade` = 1 (Medium); *Blur the sky* `blurSky` = true; *Sharp in map view* `offInMapView` = true; developer *Far
  plane* `farPlane` = 1000, *Show blur amount* `debugView` = false; legacy `tamanho` unused.
- **Tests:** `tools/loading_gate_test/` extracts the `BlurEffect` loading guards by regex, run
  `tools/loading_gate_test/run.ps1` (needs none); `tools/post_scene_test/` (order).
- **Invariants:** game MSAA on means no readable depth (Depth Blur and AO wait); depth unbound while sampled; no blur
  before the world is playable plus a 2 s hold and 1 s fade, none on a frozen screen (`kMinDepthWrites`); Uninstall
  keeps the swap while another effect requested it; `kBlurPsId` follows `kQualityTaps`.
- **Rejected:** full-resolution Gaussian replaced by half-resolution scatter-as-gather; restore-default buttons ->
  [history](../history/depth-blur.md).
- **Fragile:** the focus pass uses the fixed `kDepthA` while AO, Edge Smoothing and Picture use
  `features/post_scene.cpp:CameraDepthA`; the loading-gate regex depends on the `BlurEffect` signature and its
  `StepTime` line; the world gate of other effects depends on this file's `kSettledRegistered` initializer.

## Picture filters, colour and LUTs

Colour grading and stackable filters (film looks, glow, Emphasize, tilt-shift, CRT, LUT, colour-blind) on the finished
scene; the UI keeps its colours. Docs: [feature](../features/picture-filters.md),
[validation](../validation/picture-filters.md), [history](../history/picture-filters.md),
[LUT history](../history/2026-10-08-cube-luts.md).

- **Files:** `features/picture.cpp` (pass, scene copy, TOML, menu tabs), `features/picture.h` (`PictureParams`
  defaults), `features/picture_output.h` (RGB-only write mask), `features/cube_lut.h` (`.cube` parser, D3D-free),
  `framework/d3d9_bootstrap.cpp` (fire points), `apex_gui.cpp:PictureHeaderCard`.
- **Entry points:** `features/picture.cpp:BoundaryEffect` (post-scene 40, runs without shared depth) ->
  `Picture::OnEndScene`; `Picture::OnEndScene` again at EndScene as fallback; `Picture::BeforeOverlay`;
  `OnGameDraw` / `CopyScene` / `EnsureSceneCopy` / `CopyDepth` (DIP/DP 10); `RegisterHooks` (registered only while
  Color is on); `LoadLut` / `LoadCubeLut` (on the frame the file changes); `Picture::BeforeReset`,
  `Picture::RestartGraphics`.
- **Game hooks and addresses:** none.
- **Threads:** render for GPU work; `m_p` is guarded by `m_mutex`; `SetParams` from the menu and from config load
  (thread UNVERIFIED).
- **State:** `features/picture.cpp:gpu` (targets, scene copy, draw counters); `g_atBoundary`, `g_boundaryDone`;
  `g_depthRequested`; `features/picture.h:m_p`, `m_holdUntil`.
- **Settings:** *Color* switch `[qol.picture] enabled` = false; group switches `basic_enabled` ... `filters_enabled` =
  true; tone and colour sliders and every filter under `[qol.picture]` and `[qol.picture.filters]` with defaults in
  `features/picture.h:PictureParams` (all neutral or off); keys written by `Picture::ParamsToToml`.
- **Tests:** `tools/cube_lut_test/` (parser, PicturePS/AdaptPS compile, cube helper extracted from the source; needs
  GPU); `tools/banding_fix_check/` (Picture dither with neutral controls; needs precomp and GPU);
  `tools/post_scene_test/` (`EnsureSceneCopy`, order).
- **Invariants:** the boundary pass is the normal path (no UI mask); the end-of-frame pass never filters without a
  scene copy; the scene copy waits for `PostScene::EffectsPending`; output writes RGB only; hooks are unregistered while
  off; only Emphasize needs depth and `SceneDepthValid` guards it.
- **Rejected:** grading inside HDR output; a final-frame versus scene-copy mask as the main path (98.7% read as UI);
  Smooth gradients deband -> [history](../history/picture-filters.md), [Banding history](../history/banding-fix.md).
- **Fragile:** keys listed three times (`Picture::ParamsToToml`, `Picture::ParamsFromToml`, `kKeys`); tests read
  `PicturePS`, `AdaptPS` and `EnsureSceneCopy` text from `features/picture.cpp`; `g_atBoundary` switches the s1 input
  between the frame and the scene copy.

## Banding Fix and high-precision scene

Removes 8-bit banding with a triangular grain in every 3D-scene pixel shader, or, with *High-precision scene* (default
on), draws the scene into a 16-bit UNORM target resolved once with grain. Docs:
[feature](../features/banding-fix.md), [validation](../validation/banding-fix.md),
[history](../history/banding-fix.md).

- **Files:** `features/scene_dither.cpp` (patch "SceneDither", per-draw copy binder), `features/shader_patches.cpp`
  (`AddDither`, `AddDither2`), `framework/scene_target.cpp` (substitution and resolve),
  `framework/scene_target_logic.h` (pure decisions, `kResolveHlsl`, C++ copy of its math), `features/half_float.h`
  (`Pow2` for half decoders used by lighting code).
- **Entry points:** `features/scene_dither.cpp:OnDraw` (DIP/DP 1000) using `CopyOf` / `VsCopyOf`;
  `AcquireHooks` / `ReleaseHooks` (Install/Uninstall); `framework/scene_target.cpp:SceneTarget::Init` (first EndScene,
  from `framework/d3d9_bootstrap.cpp:FrameInit`); `ResolveAtPresent`, `ArmAtPresent`, `Ready`, `Resolve`;
  `AtBoundary` (post-scene 5); `BeforeAccess` (StretchRect, GetRenderTargetData, ColorFill detours); `AtEndScene`.
- **Game hooks and addresses:** GetRenderTargetData, ColorFill, GetRenderTarget detours through
  `framework/d3d9_extra_hooks.cpp:EnsureTargetHooks`; RT0 substitution through `D3D9Hooks::SetRenderTargetSubstitute`.
- **Threads:** render; `t_own` (thread_local) lets the module's own shader creations pass.
- **State:** `features/scene_dither.cpp:g_on`, `g_strength`, `g_copies`, `g_vsCopies`;
  `framework/scene_target.cpp:g_want`, `g_armed`, `g_resolved`, `g_tex`, `g_bb`.
- **Settings:** *Banding Fix* switch `[patches.SceneDither] enabled` = on; *Strength* `forca` = 1.0; *High-precision
  scene* `cenaAltaPrecisao` = true; developer *Show covered surfaces* (not saved); legacy `graoEmMovimento` ignored.
- **Tests:** `tools/banding_fix_check/` covers every package shader through the grain patches, run
  `tools/banding_fix_check/run.ps1 -Precomp <Shaders_Win32.precomp>` (needs precomp and GPU);
  `tools/scene_target_test/` covers `framework/scene_target_logic.h` and the resolve shader, run
  `tools/scene_target_test/run.ps1 -OutDir <dir>` (GPU part skips without a HAL device);
  `tools/crt_math_test/` checks `Pow2` (needs none).
- **Invariants:** alpha (bloom mask) is never touched; the target is A16B16G16R16, not FP16 or A2B10G10R10; refused
  with MSAA, a non-A8R8G8B8 back buffer or no 16-bit blending (`Check`); the resolve happens once, before every other
  post-scene effect and every back-buffer copy, fill or read-back; no grain on ALWAYS-without-depth-write draws, half
  grain on additive and multiply blends; per-draw grain is off while the target substitutes; jitter constant c252 is
  above every game vertex shader constant.
- **Rejected:** uniform grain, Moving grain, Smooth gradients, glow sprite smoothing, a ps_2_x half-pixel offset ->
  [history](../history/banding-fix.md).
- **Fragile:** two noise patterns (R2 per draw, IGN in `kResolveHlsl`); `framework/scene_target_logic.h:ResolveLevel`
  and `Noise` must match the HLSL; the setting lives in SceneDither while `SceneTarget::Init` runs from the bootstrap;
  a back-buffer access path outside the detours (UNVERIFIED whether the game has one) would read an unresolved frame.

## Reflections

*Water Reflections*: ponds and water without the game's own reflection mirror their shore through a screen-space ray
march against the INTZ, drawn in Night Lighting's additive water pass together with the lamp glow. Docs:
[feature](../features/reflections.md), [water pass](../features/night-lighting/water.md),
[validation](../validation/reflections.md), [history](../history/reflections.md).

- **Files:** `apex_gui.cpp` (`WaterReflectionsCard`, `SetShore`, `ShoreOn`), `features/lot_light_bridge.cpp`
  (`DrawLake`, `SetWaterFix`, classification in `Classify` and `ClassifyVs`), `shaders/water_lamps_ps.hlsl` and its
  hand-synced copy `shaders/water_lamps_hlsl.h`, `shaders/shader_ids.h` (`kLakePs`, `kLakePs2`, `kSeaNoReflPs`,
  `kSeaNoReflRainPs`, `kLakeRainPs`, `kLakeSnowPs`, `kLakeVs`, `kSeaNoReflVs`, `kLakeWeatherVs`),
  `patches/night_terrain_relight_patch.cpp` (settings, `SetShoreReflection`).
- **Entry points:** `features/lot_light_bridge.cpp:DrawLake` from `OnDrawInnerCore` (LotLightBridge DIP/DP Normal);
  `features/lot_light_bridge.cpp:SetWaterFix` (Night Lighting Present Last); `NightLighting::SetShoreReflection`
  (menu).
- **Game hooks and addresses:** none; reads the game's water vertex constants (world matrix, world-view-projection,
  camera) and uses pixel constants c20..c59, saved and restored around the pass.
- **Threads:** render.
- **State:** `features/lot_light_bridge.cpp:g_waterFix`, `g_waterRefl`, `g_waterStrength`;
  `patches/night_terrain_relight_patch.cpp:g_waterReflSetting`; `apex_gui.cpp:g_lastShore`.
- **Settings:** in `[patches.NightTerrainRelight]`, see [Night Lighting settings](night-game-side.md#settings)
  (`reflexoNoLago`, default 1.56, 0 = off).
- **Tests:** `tools/water_lamp_check/` covers the water HLSL (compile, legacy equivalence, highlight compression;
  needs GPU).
- **Invariants:** the game's water is drawn unchanged first; the extra pass is premultiplied ONE/INVSRCALPHA, RGB only,
  no depth write; INTZ is used only when it is the bound depth-stencil and is unbound while sampled;
  `DepthShare::SetInternalPass` wraps the extra draw; shore-only mode runs only while `DepthShare::Texture` exists;
  the keys belong to Night Lighting and must not be renamed.
- **Rejected:** a 40 m depth guess without readable depth; HDR lamp gain -> [history](../history/reflections.md).
- **Fragile:** the reflection depends on another effect holding the INTZ swap, but the card note checks only Depth Blur;
  `DrawLake` derives the projection A and B itself; `shaders/water_lamps_hlsl.h` is re-synced by hand; the pass runs
  mid-scene, so the INTZ holds only what was drawn before the water.

## Frame Capture (developer)

Dumps every render-target and depth switch, clear, StretchRect and draw of two frames to a text file in the Apex
folder. Docs: [feature](../features/dev-tools/frame-capture.md),
[validation](../validation/dev-tools-frame-capture.md), [history](../history/dev-tools-frame-capture.md).

- **Files:** `patches/frame_capture_patch.cpp` (compiled out with `APEX_NO_DEV_TOOLS`), observers in
  `framework/d3d9_extra_hooks.cpp`.
- **Entry points:** `patches/frame_capture_patch.cpp:Arm` (menu button *Capture two frames*, hotkey
  `Hotkeys::Action::FrameCapture`); `OnPresentBoundary` (Present Last); `OnDraw`, `ObserveClear`,
  `ObserveSetDepthStencil`, `ObserveStretchRect`, `ObserveDrawUP`; Install refused in public builds.
- **Game hooks and addresses:** none.
- **Threads:** render.
- **State:** `patches/frame_capture_patch.cpp:g` (`CaptureState`), `g_drawHooks`.
- **Settings:** `[patches.FrameCapture] enabled` only.
- **Tests:** none.
- **Invariants:** no cost while idle (draw hooks only during a capture); cancels when no Present follows 300 EndScenes.
- **Fragile:** uses the single observer slots of `framework/d3d9_extra_hooks.cpp` (`SetClearObserver`,
  `SetStretchRectObserver`, ...); a second user would overwrite them.

## Related files

- `features/bloom_alpha_probe.cpp`: developer probe armed from Night Lighting's developer page; `Request` adds
  `CaptureAtSceneEnd` at post-scene order 0 and never removes it, so the PostScene draw hooks stay on for the session
  after one capture.
- `features/compositor_readback.cpp`: CAS texture-compositor read-back deferral, part of FastCasSort, not started
  (`patches/performance_patches.cpp`); not part of the post-scene chain.

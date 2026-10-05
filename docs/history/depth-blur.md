# Depth Blur: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/depth-blur.md](../features/depth-blur.md).

### 2026-09-25: origin and the lake pass trigger

**Context:** the feature request asked for a depth of field that does not blur the UI. ReShade CinematicDOF blurred pie
menus (it runs on the final frame); REST, which can inject before a draw, crashed under DXVK (old scratchpad note
`s3ss_feature_request_dof.md`).

**Finding:** inside the D3D9 hooks the blur can run between the last scene draw and the first UI draw. An adversarial
review (item 1) found that Night Lighting's lake pass turns `ZENABLE` off and unbinds the depth-stencil, which looked
like the first UI draw and fired the effects mid-frame.

**Outcome:** the post-scene trigger was kept; extra passes that draw depth-off into the back buffer are wrapped in
`DepthShare::SetInternalPass`. The same insertion point and depth access later served Edge Smoothing, Ambient
Occlusion, the HDR sky boost of the combined build and the water reflections.

### 2026-09-27: projection model

**Context:** early notes used A = 1 and near 1.0 or 10.

**Finding:** those were 3-digit rounding. Captures (LightProbe-m80) give `d = A - near * A / z`, A = 1.00008, near 0.2 to
0.3. `farPlane` = 1000 of the Fixed curve is unrelated to the game's far plane.

**Outcome:** A = 1.00008 used as a constant; the Auto focus uses depth ratios where near cancels.

### 2026-09-28: half-resolution rewrite with Auto focus

**Context:** the old blur used a fixed Gaussian spread in half-resolution pixels (`tamanho`), so 4K got half the visible
blur of 1080p, and its composite cross-faded a fixed blur by a mask.

**Finding:**

- The Ambient Occlusion "micro dots" root cause applies here: a half-res pixel that point-samples one of four depth
  pixels picks another after a 1-pixel camera move. The Prep pass reads all four at exact texel centres.
- Every AO attempt with per-frame noise, rotation or temporal accumulation twinkled on leaves and at disocclusions.
- Spreading a fixed kernel's taps apart makes dotted artefacts; tap spacing is `r / TAPS` (at most about 1.4 half-res
  pixels at High with Blur amount 100% at 4K).
- Cross-fading a fixed blur shows a sharp image plus a ghost.
- CPU readback of the focus would stall the pipeline.

**Outcome:** rewrite with GPU Auto focus (25th percentile, eased in 1/z), 2x2 prep, scatter-as-gather with a per-pixel
radius in fractions of the screen height, FP16 linear light, bilateral upsample and GPU timing. Defaults then: Auto
focus, Blur amount 50%, Sharp area Medium, Focus speed 0.3 s, Quality High, Fixed start 0.349 and transition 0.20
(Transition moved from Advanced to the main rows), legacy spread 1.5. Configurations from before the rewrite load with
the new defaults and keep their Fixed keys. At 2160p the 50% default gave a radius close to the old default's size; at
1080p about half (computed, not observed).

### 2026-09-28: standalone baseline and precompile

**Context:** the standalone took Depth Blur from v0.1.0 commit `b84d5f1` plus the SMAA and map-view fade of
`combined-final`; the rewrite then replaced the blur. Its `post_scene.cpp` was the v0.1.0 one without camera votes,
which existed only for Ambient Occlusion; the standalone Frame Profiler had no `CameraViewProj` fallback either (frames
without the lot LOD call counted as "unknown").

**Finding:** the blur shaders could compile at start-up instead of on the render thread.

**Outcome:** the three fixed passes and all four `BlurPS` qualities are precompiled on a background thread
(`framework/shader_cache.h`). Camera votes later returned with Ambient Occlusion.

### 2026-09-28: render-callback slots

**Context:** `RenderCallbacks` had three arrays of 4 atomic slots; `Add` silently did nothing when all were taken. The
standalone without AO had exactly four `preReset` users (Depth Blur, Edge Smoothing, Lot Map Probe,
`LightmapSmooth::OnPreReset`).

**Finding:** a fifth user would lose its reset call and keep its resources across a Reset.

**Outcome:** the lists now grow as needed.

### 2026-09-28: trigger position in interiors

**Context:** the HDR diagnostic of the combined build showed depth-off back-buffer draws in the middle of interior
scenes.

**Finding:** the post-scene trigger fires at the first such draw; Picture's scene copy re-copies at every transition for
the same reason.

**Outcome:** kept as in v0.1.0 so the image does not change. Open question: fire at the last depth-tested to depth-off
transition instead. The PR #2 EndScene fallback does not change this behaviour.

### 2026-10-02: default preferences

**Context:** release 2.5.5 adopted the maintainer's player configuration as the defaults.

**Finding:** fixed focus, amount 100%, Sharp area Large, Focus speed 0.1 s, Fixed start 0.318108052, transition
0.287828237, strength 100%, Quality Medium, Glowing lights on, Blur the sky on, Sharp in map view on, far plane 1000,
legacy spread 0.800000012, debug view off. New configurations have Depth Blur on. Registered defaults, the initial state
and the reset state share the same `Params` values; saved choices are not overwritten.

**Outcome:** current defaults. The segmented-row tooltips were not updated (see the feature page).

### 2026-10-02: distant foliage speckles

**Context:** maintainer feedback with an image showing small sharp dark and light points inside blurred trees.

**Finding:** a code-based hypothesis: PrepPS stores the minimum blur amount of each 2x2 block while CompositePS compares
it to full-resolution depth. Thin alpha-tested foliage can give mismatched blur values; a low `sum w_i` lowers the
composite alpha and exposes the original pixel. Sparse gather taps are another possible source. Not confirmed.

**Outcome:** no shader change. Validate with the debug view and the same camera at several qualities before changing the
fallback, which also protects foreground edges.

### 2026-10-03: restore-default button removed

**Context:** commit 1bb2291 removed page and card restore-default controls.

**Finding:** per-row reset remains.

**Outcome:** the *Reset Depth Blur* button (`g.p = Params{}`, every field including `farPlane` and `debugView`) is gone.
The Developer controls (*Show blur amount*, *Far plane*) moved above a collapsible *Focus and rendering details* section.

### 2026-10-03: post-scene effects with hidden game UI

**Context:** commit abd4603. With the game UI hidden there may be no depth-off draw after the scene.

**Finding:** the post-scene chain never fired.

**Outcome:** EndScene fallback before Picture's scene copy and the overlay; boundaries with a mismatched depth-stencil
are rejected without consuming the effects. The chain also waits for shader precompilation (commit 4107eee) so loading
frames are not blocked.

### 2026-10-05: loaded-world guard

**Context:** commit f575f0a. Loading screens and menus could leave stale depth that the blur sampled.

**Finding:** the WorldManager (`+0x41` active, `+0x1B4` modes 1 to 3) identifies an active world read-only.

**Outcome:** Depth Blur skips all passes outside an active world and needs 3 s of continuous activity to resume. A load,
missing or unreadable manager, video reset or stopped depth swap invalidates readiness; Auto focus snaps on return;
map fade and frame timing are reset. The shared INTZ resource stays available to other consumers.

### 2026-10-05: loading-window gate

**Context:** same commit. The loaded-world flag can become true while the loading UI is still attached.

**Finding:** the native UI root holds the loading/startup window id `0x95947678` (created at `0x00EC7DB9`, removed by the
callback at `0x00EC7A60` on Steam). The UI service getter is reachable from the `UIManager_GetMainWindowImpl` call
signature and has the shape `mov eax, [global]; ret`.

**Outcome:** the shared gate also requires that window to be absent, failing closed on missing UI state or an
unrecognised getter. The menu's readiness interval starts only after the window is gone, and `BlurEffect` re-checks
immediately. 91 mock checks pass; real transitions and non-Steam builds are not yet validated.

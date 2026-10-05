# Picture filters: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/picture-filters.md](../features/picture-filters.md). HDR output is described in
[removed-features.md](../removed-features.md).

### 2026-09-28: grading inside HDR output

**Context:** the combined build added grading controls to HDR output (raw notes "HDR (28/09)"): exposure, contrast,
blacks, expansion start, temperature, deband.

**Finding:** deband by random sampling would add noise; fixed rings (2 x 8 directions) with a threshold, applied to the
scene only, keep UI edges sharp.

**Outcome:** deterministic deband kept.

### 2026-09-28: Picture split from HDR

**Context:** raw notes "HDR nativo e saida HDR (28/09, noite)". The grade had to work with or without HDR.

**Finding:** the grade could share the HDR pass in `hdr_output.cpp/.h` (`HdrOutput::RenderPictureUI`, `PictureParams`,
shader `HdrPS` with constants c0..c15 including HDR-only `cNits`, `cSky`, `cCal`, `cLook.y` sRGB option, `cColor.xy`,
`cGrade.w` expansion start, `cWb.w` output mode). `HdrOutput::BeforeOverlay` ran from `HookedEndScene` right after
`RenderCallbacks::endSceneBeforeOverlay`; `HdrOutput::OnEndScene` ran after the S3SS overlay, before `original_EndScene`.
`HdrOutput::BeforeCreateDevice` could parse the whole TOML early when the device was created before the config load.

**Outcome:** a "Picture" section on the Display tab with its own table `[qol.picture]`; old grading values were read from
`[qol.hdr]` when `[qol.picture]` was missing (`ReadGrade`), and `SaveToToml` stopped writing grade keys to `[qol.hdr]`.
The combined build's README still listed the controls under "HDR > Advanced" (stale).

### 2026-09-28: 98.7% of the screen treated as UI

**Context:** HDR_Diag_1/2: grading never applied in interiors.

**Finding:** the first version copied the scene at the first depth-off draw after the scene. Interiors have depth-off
draws in the middle of the scene, so the copy missed most of the lamp light and the mask marked almost everything as UI.
Separately, hooks at `Normal` priority never saw draws that Night Lighting replaced and ended with `Skip`. Copying before
the bloom strip (FrameCapture 24/09 #977: one full-screen 2-primitive strip right after the scene) would leave bloom
outside the copy.

**Outcome:** the copy is retaken at every depth-on to depth-off transition (last wins), after the bloom strip, with hooks
that see every draw.

### 2026-09-28: carve-out for the standalone

**Context:** HDR output and Native HDR were removed from the standalone (plan `PLANO-SEPARACAO.md`, section 2d).

**Finding:** kept: `PictureParams`, the Picture part of the shader, the scene-copy machinery, `BeforeOverlay` logic, the
frame / scene / chain targets, shader and queries, the SDR path of `OnEndScene`, `[qol.picture]` read and write with the
`[qol.hdr]` fallback (moved to the one-time `S3SS.toml` migration), the UI. Dropped: the HDR aux shaders (BrightPS,
DownPS, UpPS, ControlPS), the glow and limiter targets, `s_lampGain`, `HdrNative::Update`, display queries, the depth
request of the sky boost, HDR_Diag, the device-creation path (`BeforeCreateDevice`, `AfterCreateDevice`,
`ApplyColorSpace`, `AfterReset`, `QueryDisplay`), DXVK and DXGI headers, and `HdrOutput::LampGain()` in the lot light
bridge. For SDR, running at Apex's own EndScene is correct in either hook order with S3SS.

**Outcome:** `features/picture.cpp` (SDR only, shader `PicturePS`, constants renumbered to c0..c12, log prefix
`[Picture]`, registry name `"Picture"`), precompiled on a background thread.

### 2026-09-30: Smooth gradients moves to the Banding tab

**Context:** the Color page gained a Banding tab grouping everything against colour steps.

**Finding:** the deband is the only help for ps_2_x surfaces such as the sky.

**Outcome:** Smooth gradients follows the Banding Fix switch (`Effective()`): with Picture off the pass runs with only
the deband, never in a frame without a scene copy. At the time, *Reset Picture* kept the deband value.

### 2026-09-30: draw-hook order after the post-scene trigger

**Context:** in the combined build, Picture's copy and the post-scene trigger were both `Priority::First` draw hooks and could fire on the same
draw in an undefined order; post-scene pixels would then be treated as UI.

**Finding:** an explicit priority removes the race.

**Outcome:** by 2.1.0, Picture's draw hooks run at priority 10: after the post-scene trigger, before `Early` (25) and every feature
that may skip a draw (`Normal`, 50).

### 2026-10-03: restore-default buttons removed; Film tones and Color mixer collapsed

**Context:** commit 1bb2291 removed page and card restore-default controls; commit 986cabb grouped rare controls.

**Finding:** per-row reset (changed dots) remains.

**Outcome:** *Reset Picture* (`PictureParams{}` keeping `enabled`, `compare` and the deband) and *Reset mixer* (all six to
100%) are gone. Film tones and Color mixer moved into collapsed *Advanced* groups.

### 2026-10-03: filtered screenshots and loading frames

**Context:** commits fc88731, 917f1d8 and 4107eee added the filtered player screenshot and fixed captures taken with the
menu open.

**Finding:** with the menu open the capture fired before the Picture pass and missed the grade; on loading frames the
pass could wait on shader compilation.

**Outcome:** the bootstrap runs Picture once and fires `filteredSceneBeforeOverlay` before the menu; the pass returns
until shader precompilation completes.

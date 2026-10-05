# Frame Capture: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/dev-tools/frame-capture.md](../features/dev-tools/frame-capture.md).

### 2026-09-24: the first frame capture

**Context:** Depth Blur needed to know where the game finishes the 3D scene and starts drawing its UI, and whether the
scene depth buffer could be read. The combined build registered the patch `FrameCapture` ("Frame Capture
(developer)") with its `APEX_REGISTER_FEATURE` inside `#ifndef S3SS_PUBLIC`, so the public build had no such patch. It
was shown under Apex tab > Performance > "Developer tools" (a collapsing header in `gui.cpp`, `RenderApexFeature`;
`IsApexPatch` listed it so it appeared in the Apex tab, not the Patches tab), with "Capture now", a Ctrl+Shift+F9
chord, and `enabled` saved in `S3SS.toml` by `OptimizationPatch::SaveToToml`. Output:
`Documents\Electronic Arts\<localized>\S3SS\S3SS_FrameCapture.txt`.

**Finding:** the capture (2,381 lines, 1,154 draws per frame, still on disk) showed the scene drawn into the back
buffer with the auto depth-stencil `S1` (3840x2160 D24S8, 8x MSAA at the time); readable depth `INTZ: YES`,
`DF24/DF16/RESZ: NO`, `D24S8 as a texture: YES` under DXVK; `StretchRect BACKBUFFER -> S12`, a bloom chain (`#973` into
a 1024x1024 target, `#974..#976` ping-pong between `S13` and `S12`), then `#977`, the bloom composite strip, with UI
draws from `#978`. The file was written with Portuguese labels (`Data`, `Versao do jogo`, `COR`, `filtro`,
`EndScene do jogo (daqui pra baixo e o overlay do S3SS)`); later code writes English labels.

**Outcome:** the Depth Blur INTZ swap (`patches/depth_blur_patch.cpp` cited "see S3SS_FrameCapture.txt analysis"). The
`#977` rule ("the game adds its bloom: one full-screen DrawPrimitive strip right after the scene") was cited by
`hdr_output.cpp` and used by PostScene and HDR to find the scene/UI boundary. MSAA depth cannot be read in D3D9, so
Depth Blur and everything using its INTZ depth requires the game's own Edge Smoothing off; check
`Present params ... ms=` in any new capture.

### 2026-09-25: lake pass with depth off

**Context:** review at about 03:30, item 1.

**Finding:** the lake pass turned ZENABLE off, and Depth Blur treated it as the first UI draw.

**Outcome:** fixed with `DepthShare::SetInternalPass`. A frame capture shows such mid-scene z-off draws immediately.

### 2026-09-28: the first z-off draw is not always the UI

**Context:** HDR diagnostics (NOTAS "HDR (28/09)", "Diagnosticos HDR_Diag_1/2").

**Finding:** interiors have z-off draws in the middle of the scene; copying the scene at the first z-off draw lost the
lamp light.

**Outcome:** the HDR copy was refreshed at every z-on to z-off transition (the last one wins) and included the bloom
strip right after the scene. HDR output was later removed ([removed-features.md](../removed-features.md)); the lesson
stands: read the capture rather than assuming a single boundary.

### 2026-09-28: standalone plan

**Context:** PLANO-SEPARACAO.md section 4.

**Finding:** the standalone needed its own output path and a decision about the single-owner ExtraHooks observer slots.

**Outcome:** the output became `Apex Radiance\ApexRadiance_FrameCapture.txt` with the table `[patches.FrameCapture]` in
`ApexRadiance.toml`. The observer slots stayed single-owner, with Frame Capture as the only user. Recording all 16
samplers and a content hash of the shaders, to match ids with Light Probe captures, remains an idea only.

### 2026-10-02: one build, developer mode

**Context:** the separate developer build was replaced by one unified ASI ([developer-mode.md](../features/developer-mode.md)).

**Finding:** Frame Capture had to stay unavailable in normal play.

**Outcome:** `Install` refuses in normal mode, `PatchManager::LoadFromToml` skips its table, and its shortcut row
appears only in developer mode. It moved to Developer > Captures and its shortcut became the `FrameCapture` action of
the shortcut presets (`F`, `7`, `F5`). The patch description and install log still mention Ctrl+Shift+F9.

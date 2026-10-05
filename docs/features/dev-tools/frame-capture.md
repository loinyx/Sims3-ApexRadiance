# Frame Capture

Frame Capture writes a draw-by-draw log of two consecutive frames to `ApexRadiance_FrameCapture.txt`. It records every
render-target and depth-stencil switch, Clear, StretchRect, BeginScene, EndScene and every draw (including the UP
variants) with its bound render targets, depth surface, shaders, first three textures, depth, stencil, blend,
alpha-test and colour-write states and viewport. Identical consecutive draws are folded into runs, and resources get
short per-capture ids (`S12`, `T141`, `PS41`) with a one-line definition at first sight. It costs nothing while idle.

## Status

| | |
|---|---|
| Availability | Developer mode only. Released in 2.5.5 as part of the unified build (earlier: developer build only) |
| Default | Off |
| Menu | Developer > Captures > Capture two drawn frames (header switch, Capture two frames, advanced "File and shortcut") |
| Configuration | `[patches.FrameCapture]` in `ApexRadiance.toml`; shortcut `[ui] frame_capture_key` (see [ui.md](../../ui.md#shortcuts)) |
| Source | [`patches/frame_capture_patch.cpp`](../../../patches/frame_capture_patch.cpp) |

## The problem

The Light Probe ([light-probe.md](light-probe.md)) only sees screen-sized draws covering one pixel. Building post-scene
effects needs the frame's whole structure at the D3D9 level: where the game finishes the 3D scene and starts its UI,
which surfaces it copies, whether the scene depth buffer can be read, and which mid-scene passes turn depth off.

## How Apex Radiance solves it

While armed, Frame Capture hooks the D3D9 calls at the end of every hook chain and formats one line per event or draw
for two frames, starting at the next Present. The file shows the order of passes, so effects such as Depth Blur and the
post-scene chain can find the scene/UI boundary and the readable depth format from evidence rather than assumptions.

### Facts established with it

- The scene is drawn straight into the back buffer with the auto depth-stencil (a D24S8 surface, multisampled when the
  game's own anti-aliasing is on: `Present params: ... ms=8`).
- Readable depth under DXVK: `INTZ: YES`, `DF24/DF16/RESZ: NO`, `D24S8 as a texture: YES`, which is the basis of the
  Depth Blur INTZ depth share.
- The end of the scene: `StretchRect BACKBUFFER -> S12` (a 2048x1024 render target, texture `T141`, also the water
  refraction copy), a bloom chain of full-screen strips into a 1024x1024 target and ping-pong between `S13` and `S12`,
  then the bloom composite: `DP type=5` (triangle strip), 2 primitives, into the back buffer, `z=0/0`, `blend=1`,
  `cw=7` (RGB only), sampling `T141`. Back-buffer draws with Z off after it are the UI.
- The game's EndScene marker comes after all game draws; everything after it is the Apex overlay, the Picture pass and
  other overlays.
- The first z-off back-buffer draw is not always the UI: interiors and the lake pass have z-off draws in the middle of
  the scene. Read the capture rather than assuming a single boundary.

## Settings

| Control | TOML | Type | Default | Notes |
|---|---|---|---|---|
| Capture two drawn frames (header switch) | `[patches.FrameCapture] enabled` | bool | false | Category Experimental; ignored in normal mode (`PatchManager::LoadFromToml` skips the table) |
| Capture two frames | - | button | - | Arms a capture (`Arm()`) |
| Frame Capture shortcut | `[ui] frame_capture_key` | key chord | Preset: `F`, `7` or `F5` | Polled in `OnEndScene` while installed; listed in Settings > Shortcuts only in developer mode |
| Status | - | text | - | `Ready`, `Waiting for the next frame...`, `Capturing...`, `Capture saved (N lines) to ApexRadiance_FrameCapture.txt`, `ERROR: could not write the file`, `ERROR: the game does not call IDirect3DDevice9::Present, capture cancelled` |

In normal mode `Install` fails with "Enable developer mode and restart the game first".

## Compatibility and interactions

- **Owns the single ExtraHooks observer slots** for Clear, SetDepthStencilSurface, StretchRect and the UP draws.
  Installing another observer would silently replace Frame Capture's, or the reverse; the Frame Profiler does not count
  UP draws for this reason.
- `Priority::Last` on the draw hooks: a draw that an earlier hook replaced and skipped (the lot light bridge, blanked
  Light Probe textures) is not logged as the game's call; the replacement draw issued by the other module re-enters the
  registry and is logged instead (inferred from the hook registry, where `Skip` ends the chain). Draw numbers include mod
  draws.
- Post-scene effects (Edge Smoothing, Ambient Occlusion, Depth Blur) draw inside the frame; turn them off to see the
  vanilla frame.
- Draw indices (`#n`) count every draw including the overlay; the Light Probe's `#n` counts only screen-sized draws.

## Limitations

- Only texture stages t0..t2 and render targets 0 and 1 are recorded; no shader constants (use the Light Probe).
- Ids are per capture; a draw index such as the bloom composite's is not a game constant. The rule is "the first
  full-screen z-off strip into the back buffer right after the scene".
- At most 80,000 lines (enough for two frames of about 1,200 draws).
- Shaders get no definition line; use the Light Probe to dump the bytecode of one.
- The file is overwritten by each capture; it is not a Report a problem capture folder.

## Technical reference

### Lifecycle

All state lives in an anonymous namespace (`CaptureState g`).

1. **Install** registers, all at `Priority::Last` under the name `FrameCapture`: `RegisterBeginScene` (which also calls
   `ExtraHooks::EnsureInstalled`), `RegisterSetRenderTarget`, `RegisterDrawIndexedPrimitive`, `RegisterDrawPrimitive`
   and `RegisterPresent`; adds `OnEndScene` to `RenderCallbacks::endSceneBeforeOverlay` (fired inside the game's
   EndScene before the overlay draws); sets the four ExtraHooks observers. Log `[FrameCapture] Installing...` then
   `[FrameCapture] Installed (press the button or Ctrl+Shift+F9 to capture)`.
2. **Arming**: the button or the shortcut sets `armed`. If 300 EndScenes pass while armed and no Present was ever seen,
   the capture is cancelled (log `[FrameCapture] No Present seen while armed`).
3. **Start** at the next Present (`OnPresentBoundary` -> `StartCapture`): clears state, writes the header and
   `==== FRAME 1 ====`.
4. **Recording**, `kFramesPerCapture = 2`:
   - Events (`Event()` flushes the pending draw run and definitions first): `BeginScene`,
     `SetRenderTarget[i] = <id>`, `SetDepthStencilSurface = <id>` (the surface the game requested, before any Depth
     Blur substitution), `Clear [COLOR Z STENCIL] rt0= ds= color=0x.. z= rects=`, `StretchRect <src> -> <dst>
     filter=<n>`, and `---- Game's EndScene (everything below: the Apex overlay, the Picture pass, other overlays) ----`.
   - Draws (`OnDraw`, from the DIP and DP hooks and the UP observer, kinds `DIP`, `DP`, `DPUP`, `DIPUP`): a signature
     `kind type rt0 rt1 ds ps vs t0 t1 t2 z=ZENABLE/ZWRITE/fZFUNC st=STENCIL blend=ALPHABLEND atest=ALPHATEST
     cw=COLORWRITE(hex) vp=X,Y WxH`. Consecutive draws with the same signature become one line
     `#a-#b <sig> xN prims=<sum>`.
   - At each Present: `==== Present (end of frame N, D draws) ====`, then `==== FRAME N+1 ====` or the file write.
5. **Write** (`WriteFile`): `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance_FrameCapture.txt`
   (`ApexPaths::ApexDirectory()`), truncated; after `kMaxLines = 80000` lines it appends
   `(capture truncated at 80000 lines)`. Log `[FrameCapture] Capture saved (N lines) to ApexRadiance_FrameCapture.txt`.
6. **Uninstall** writes a capture in progress, unregisters everything and clears the observers.

### Ids and definitions

Every object gets an id the first time it appears: surfaces `S<n>`, textures `T<n>`, pixel shaders `PS<n>`, vertex
shaders `VS<n>`. Counters restart with every capture. Surfaces and textures get a `[def]` line just before the event or
draw that introduced them:

```
    [def] S12 = 2048x1024 A8R8G8B8 usage=RT pool=0 ms=0 (texture level of T141)
    [def] T141 = tex2D 2048x1024 A8R8G8B8 usage=RT mips=1
    [def] S1 = 3840x2160 D24S8 usage=DS pool=0 ms=8
```

The back buffer is named `BACKBUFFER`, and a surface equal to it gets `<-- BACKBUFFER`.

### Header

`Apex Radiance Frame Capture`, `Date:`, `Game version:` (`GetGameVersionName()`),
`Device: adapter= type= behavior=0x.. (PUREDEVICE=yes|no)`,
`Present params: WxH fmt= count= ms= msq= swap= windowed= autoDS= dsfmt= flags= interval=`,
`BACKBUFFER = WxH fmt ms=`, `Depth-stencil bound at the start: <id>`, then `Readable depth support (depth-stencil
texture):` with `CheckDeviceFormat` results for INTZ, DF24, DF16 (depth-stencil textures), RESZ (render-target surface)
and D24S8 as a texture, and the legend line.

### Files and functions

| File | Symbol | Role |
|---|---|---|
| `patches/frame_capture_patch.cpp` | `FrameCapturePatch` (`ApexPatch("FrameCapture")`) | Install, Uninstall, custom UI |
| | `OnDraw`, `FlushRun`, `Event`, `FlushDefs`, `AddLine` | Recording and run folding |
| | `SurfId`, `TexId`, `ObjId`, `NewId`, `FmtName`, `UsageStr` | Ids and definitions |
| | `WriteHeader`, `WriteFile`, `StartCapture`, `OnPresentBoundary`, `OnEndScene`, `Arm` | Lifecycle |
| | `ObserveClear`, `ObserveSetDepthStencil`, `ObserveStretchRect`, `ObserveDrawUP` | ExtraHooks observers |
| `framework/d3d9_extra_hooks.cpp/.h` | `ExtraHooks::EnsureInstalled`, `Set*Observer` | Detours on vtable slots 34 StretchRect, 39 SetDepthStencilSurface, 40 GetDepthStencilSurface, 43 Clear, 83 DrawPrimitiveUP, 84 DrawIndexedPrimitiveUP; one observer slot each |
| `framework/render_callbacks.h` | `endSceneBeforeOverlay` | EndScene callback list |
| `framework/patch_base.cpp` | `PatchManager::LoadFromToml` | Skips `[patches.FrameCapture]` in normal mode |
| `apex_gui.cpp` | Developer > Captures `FeatureCard("FrameCapture", ...)`; Shortcuts rows | UI |

No game addresses; D3D9 level only.

## Rejected approaches

- Taking the first z-off back-buffer draw as the start of the UI
  ([history](../../history/dev-tools-frame-capture.md#2026-09-28-the-first-z-off-draw-is-not-always-the-ui)).

## See also

- [Validation](../../validation/dev-tools-frame-capture.md)
- [History](../../history/dev-tools-frame-capture.md)
- [Developer mode](../developer-mode.md), [Light Probe](light-probe.md), [Depth Blur](../depth-blur.md)

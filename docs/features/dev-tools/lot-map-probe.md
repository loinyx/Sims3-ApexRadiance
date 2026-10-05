# Lot Map Probe

Lot Map Probe was a development study tool that re-rendered the game's own scene draws from a top-down orthographic
camera to build a lot height map and a top-down colour image. It tested whether a world-space ambient occlusion could
be built from the game's geometry. It exists only in the frozen combined build; Apex Radiance does not contain it. This
page keeps the technique, "redraw the scene from another camera with every hook bypassed", as a reference.

## Status

| | |
|---|---|
| Availability | Removed. Never part of Apex Radiance; the code exists only in the combined build (`patches/lot_map_probe_patch.cpp`, inside `#ifndef S3SS_PUBLIC`) |
| Default | Not applicable |
| Menu | None in Apex Radiance (combined build: Apex tab > Performance > Developer tools) |
| Configuration | None in Apex Radiance (combined build: `[patches.LotMapProbe] enabled`, default false, category Experimental) |
| Source | Combined build only (see [architecture.md](../../architecture.md)) |

## The problem

A world-space ambient occlusion needs a height map of the lot. The question was whether the game's own scene draws
(terrain, houses, furniture, trees, Sims) can be re-rendered from above with the game's shaders, textures and vertex
data, without reimplementing any of them.

## How Apex Radiance solves it

It does not ship this tool. The study showed that the redraw works (every qualifying draw redrawn, full coverage), but
shader families keep the camera in different constant blocks, roofs cover interiors from above, and Sims would need a
redraw every frame. Ambient occlusion uses screen-space GTAO instead ([ambient-occlusion.md](../ambient-occlusion.md)).

## Settings

None in Apex Radiance. The combined build had:

| UI label | TOML | Type | Default | Notes |
|---|---|---|---|---|
| Lot Map Probe > Enabled | `[patches.LotMapProbe] enabled` | bool | false | Category Experimental, developer build only |
| Capture lot map | - | button | - | Disabled while busy; starts a 120-frame countdown ("close the menu within 2 s") |
| Status | - | text | - | `Ready`, `Starting in 2 s: close the menu`, `Capturing: turn the camera slowly around the lot...`, `Saving...`, `Saved to MapaLote_N (R draws redrawn, P% covered)`, `Failed: the game's camera was not found (are you in Live mode?)`, `Failed: not enough video memory for the map`, `Failed to copy the map`, `Failed to save the files`, `Cancelled (video change)`, `Off` |

No shortcut. Instructions shown: click, close the menu within 2 s and turn the camera slowly around the lot for about
4 s.

## Compatibility and interactions

In the combined build:

- `Priority::First` on the draw hooks: it saw the game's draw before Night Lighting's bridge replaced it, so the map
  used the game's original shaders.
- It bypassed Depth Blur's INTZ swap (raw depth calls) and every other hook (`CallOriginal*`), so the Frame Profiler,
  Frame Capture and the Light Probe did not see its extra draws.
- A device Reset cancelled the capture (`preReset`).

## Limitations

- Needed Live mode with a normal scene camera (the vote finds no camera otherwise).
- A 160 m square fixed at capture start; only geometry that the game draws while the camera turns gets in.
- Roughly doubled the scene's draw calls for about 4 s.
- Roofs cover interiors from above, and a static map cannot shade moving Sims.
- Height calibration was never validated ([validation](../../validation/dev-tools-lot-map-probe.md)).

## Technical reference

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `kMapSize` | 2048 | Map resolution (colour render target + INTZ depth) |
| `kMapHalf` | 80.0 m | Half extent: the map covers 160 x 160 m |
| `kAheadM` | 25.0 m | Map centre this far ahead of the camera, horizontally |
| `kAboveCamM` | 20.0 m | Top of the height range above the camera |
| `kRangeM` | 300.0 m | Height range |
| `kCountdownFrames` | 120 | Countdown before capturing |
| `kCaptureFrames` | 240 | Capture length (about 4 s) |
| `kCamBlocks` | {0, 4, 40, 180, 192, 216} | VS constant blocks where shader families keep the camera (from Light Probe captures) |

### Hooks and flow

Hooks (name `LotMapProbe`, all `Priority::First`): Present (`OnFrameBoundary`), SetRenderTarget (tracks RT0 when not
inside its own call), DrawIndexedPrimitive and DrawPrimitive (`OnGameDraw`), plus `RenderCallbacks::preReset`
(`OnPreReset` cancels and frees everything). State lives in `State g`.

1. **Camera vote** (`OnGameDraw`, every game draw during countdown and capture): only draws into the back buffer with
   ZENABLE on. Reads VS c0..c255; each block in `kCamBlocks` that `LooksLikeCamera` (row 3 unit length = view forward;
   rows 0 and 1 longer than 0.3 and orthogonal to it and to each other) votes, up to 32 candidates, compared on rows x,
   y and w only (`SameXYW`), because the z row differs between draws of the same frame (near 0.2..1.0, 10). A
   candidate with at least 8 votes and the most votes is the frame's camera; at Present it becomes `prevCam`, the
   camera for the first draws of the next frame.
2. **Start** (`OnFrameBoundary`, countdown at 0): needs `prevCam`; creates the 2048 INTZ depth texture
   (`D3DUSAGE_DEPTHSTENCIL`, DEFAULT pool), a 2048 A8R8G8B8 render target (`CallOriginalCreateRenderTarget`, so other
   hooks do not see it) and a small ps_3_0 copy shader (`D3DCompile` of `tex2Dlod(sDepth, uv).r`); builds the ortho
   matrix once (`SetupMap`).
3. **SetupMap**: from the camera rows (row0 = Px x right, row1 = Py x up, row3 = forward, the game's projection layout)
   recovers R (rows normalised), t, the camera position `camPos = -(R^T t)` and forward; world up sign `upSign` = sign
   of R[1][1]; centre = camera xz + 25 m along the horizontal forward; `hTop = upSign*camPos.y + 20`. Ortho
   view-projection rows: `(1/80, 0, 0, -cx/80)`, `(0, 0, 1/80, -cz/80)`, `(0, -upSign/300, 0, hTop/300)`,
   `(0, 0, 0, 1)`: x maps to world x, y to world z, depth = (hTop - upSign*y)/300.
4. **Redraw** (each qualifying game draw while capturing, just before the game's own draw): finds every 4-register
   block in VS c0..c252 whose x, y and w rows equal the frame camera (`camRegs`; draws with none are skipped as "other
   camera"); skips draws without Z write and draws with a second render target. It saves 10 render states, binds its
   colour target (`CallOriginalSetRenderTarget`), its INTZ surface (`ExtraHooks::RawSetDepthStencilSurface`, bypassing
   Depth Blur's substitution) and a 2048 viewport (`CallOriginalSetViewport`); sets cull none, ZFUNC LESSEQUAL, no
   blending, colour writes 0xF, stencil, scissor and clip planes off, no depth bias, no sRGB write; clears once per
   capture (after scissor is off, because Clear honours it); writes the ortho matrix into every camera block
   (`CallOriginalSetVertexShaderConstantF`); issues the draw with `CallOriginalDrawIndexedPrimitive` /
   `CallOriginalDrawPrimitive`; then restores constants, states, render target, depth surface and viewport (viewport
   last, because setting the render target resets it). The game's shaders, textures, vertex data and alpha test are
   untouched, so leaf cut-outs stay.
5. **Accumulation**: the map is not cleared between frames; the game only draws what is in view, so turning the camera
   fills it.
6. **Dump** (`Dump`, at the first game draw after the 240 frames): copies INTZ to an R32F render target with the copy
   shader (a pre-transformed `D3DFVF_XYZRHW | D3DFVF_TEX1` quad through `DrawPrimitiveUP`, point sampling),
   `GetRenderTargetData` for depth and colour, restores state with a `D3DSBT_ALL` state block applied twice (again
   after the render-target reset) and puts the shaders and texture back through the hooked setters so tracking modules
   see the game's own again. Then writes the folder.

### Output

`Documents\Electronic Arts\<localized>\S3SS\MapaLote_N\` (N = first free number):

| File | Content |
|---|---|
| `altura.f32` | Raw 2048x2048 float32 depth, row-major, top row first (16 MB). Height = hTop - depth x 300 (in `upSign*y` units); depth 1.0 = nothing drawn |
| `altura.bmp` | 24-bit BMP, brightness = height stretched over the covered range (brighter = higher); nothing drawn = dark blue (B = 60) |
| `cor.bmp` | 24-bit BMP of the top-down colour target |
| `info.txt` | Map size, extent and centre, depth-to-height mapping, starting camera position and forward, frames, redrawn and failed counts, skips (other camera, no depth write, multiple targets), coverage %, depth min/max and heights, and `camera found in registers (draws): c<r>=<count> ...` |

Depth values span a tiny range, so `altura.bmp` stretches min..max; use `altura.f32` for numbers.

### Files and functions (combined build)

| File | Symbol | Role |
|---|---|---|
| `patches/lot_map_probe_patch.cpp` | `LotMapProbePatch` | Install, Uninstall, UI |
| | `LooksLikeCamera`, `SameXYW`, `Same` | Camera recognition |
| | `SetupMap` | Ortho camera |
| | `Redraw<DrawFn>` | Second draw into the map |
| | `OnGameDraw`, `OnFrameBoundary`, `OnPreReset` | Lifecycle |
| | `CreateResources`, `ReleaseResources`, `Dump`, `WriteBmp` | Resources and output |
| `d3d9_hook_registry.h` | `CallOriginal*` | Device calls that bypass all hooks |
| `d3d9_extra_hooks.h` | `RawGetDepthStencilSurface`, `RawSetDepthStencilSurface` | Real depth surface, no Depth Blur substitution |
| `gui.cpp` | `RenderApexFeature("LotMapProbe", ...)` under "Developer tools" | UI |

No game addresses. The camera layout comes from Light Probe captures: VS c40..c43 = world view-projection in every
scene draw, with other families using c0, c4, c180, c192 and c216; projection row2 = A*row3 + (0, 0, 0, -near) with
A = 1.00008 ([light-probe history](../../history/dev-tools-light-probe.md)).

## Rejected approaches

- A lot height map as the basis of ambient occlusion
  ([history](../../history/dev-tools-lot-map-probe.md#2026-09-27-world-space-ao-study)).
- Replacing only VS c40..c43 with the map camera
  ([history](../../history/dev-tools-lot-map-probe.md#2026-09-27-only-c40c43-replaced)).
- Voting on any constant block, including all-zero blocks
  ([history](../../history/dev-tools-lot-map-probe.md#2026-09-27-a-block-of-zeros-won-the-camera-vote)).

## See also

- [Validation](../../validation/dev-tools-lot-map-probe.md)
- [History](../../history/dev-tools-lot-map-probe.md)
- [Ambient Occlusion](../ambient-occlusion.md), [Light Probe](light-probe.md)

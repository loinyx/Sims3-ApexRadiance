# Light Probe (light capture)

The Light Probe records everything the GPU does to one screen pixel. With the mouse over a spot and the capture
shortcut pressed, Apex Radiance lists every draw call of the next frame that paints that pixel, with its shaders
(bytecode and disassembly), render states, textures, sampler filters and non-zero shader constants, the final screen
colour, and what Night Lighting did with each draw. Players use it as **Light capture** on the Report a problem page;
in developer mode the Developer page can also replace any recorded texture with solid black or white, live, to see what
it contributes.

## Status

| | |
|---|---|
| Availability | Released in 2.5.0 as a player capture (Report a problem). Texture replacement: developer mode |
| Default | Always available while Night Lights is installed; no switch |
| Menu | System > Report a problem > Save a capture ("Capture the light at a spot", shows the key). Developer > Lighting > Inspect lighting state > Light probe textures |
| Configuration | No feature table. Shortcut `[ui] probe_key` in `ApexRadiance.toml` (see [ui.md](../../ui.md#shortcuts)) |
| Source | [`features/light_probe.cpp`](../../../features/light_probe.cpp), [`features/light_probe.h`](../../../features/light_probe.h) |

## The problem

Reverse engineering the game's lighting paths statically repeatedly led to wrong conclusions: the same surface can be
painted by several passes (a base pass and a modulate-2x lot light pass, snow on top of stairs), each shader family keeps
its lamps and camera in different constant registers, and a pixel that looks "dark" may be a texture, not a light
problem. The reliable method is to look at the exact pixel that is wrong, list the draws that paint it, read the shader
and the constants the game actually uploaded, and compare them with a pixel that is right.

## How Apex Radiance solves it

The probe tests each draw of one frame against a 1x1 scissor rectangle at the chosen pixel with an occlusion query, so
only the draws that really reach that pixel are reported.

1. **Request.** The Probe shortcut (Ctrl+Shift+V, 4 or F7 by preset) maps the cursor to a back-buffer pixel and arms
   the probe.
2. **Record.** In the next frame, every draw into a screen-sized render target is recorded: textures, filters, shaders,
   render states, constants and Night Lighting's description of the draw. A second copy of the draw, with colour and
   depth writes off and a 1x1 scissor, runs inside an occlusion query.
3. **Finish.** At the following Present the queries are read; draws with samples > 0 are written to
   `Light capture.txt` with their shaders and textures, in a new dated capture folder.
4. **Follow-ups.** After a capture by shortcut, the same pixel is captured again 1 s and 3 s after any loaded lot
   changes the story it shows, up to 6 automatic captures within 2 minutes (for floor-switch problems).
5. **Compare.** Two consecutive captures (piece A, then piece B) end with a register-by-register comparison of the
   object draw chosen at each pixel.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Settings > Shortcuts > Report a problem > Light capture | `[ui] probe_key` | key chord | preset key (Ctrl+Shift+V / Ctrl+Shift+4 / Ctrl+Shift+F7) | any allowed chord | Starts a capture of the pixel under the mouse. Empty = use the preset |
| Report a problem > Include a screenshot | `[ui] capture_screenshot` | bool | on | | Adds `Screenshot.png` to each capture folder |
| Developer > Light probe textures > Replace with white (unticked = black) | (not saved) | bool | off | | Colour used for replaced textures |
| Developer > Light probe textures > `T<n>: <desc>` | (not saved) | bool | off | | Replaces that texture in every draw that binds it, until unticked |
| Developer > Light probe textures > Untick all | (button) | | | | Restores every texture |

## Compatibility and interactions

- **Host:** the probe runs from Night Lighting's Present hook (`NightTerrainRelight`). With Night Lights off there is
  no capture, and the Report page says so ("The recording and the two lighting captures need Night Lights on").
- **Report a problem:** each capture is a normal capture folder (log, settings, crash report, screenshot); inside an
  open capture session it goes into the session folder. See [bug-reports.md](../bug-reports.md).
- **Recorder:** every capture adds a `[probe]` line to a running recording ([recorder.md](recorder.md)).
- **Night Lighting detail:** `LotLightBridge` fills its per-object detail only while `LightProbe::Capturing()` is true,
  so it costs nothing otherwise.
- **Hook chain:** the probe hooks draws at `Priority::Last`. A draw that the bridge replaced and `Skip`ped is seen as
  the bridge's own re-issued draw, hence the `mod draw` wording. The 1x1 query copy goes through the hook registry
  again, so hooks with a higher priority (bridge, Frame Profiler counters, census) see each recorded draw twice during
  the capture frame (inferred from the registry design; harmless for rendering, visible in per-frame counters).
- **Texture replacement** returns `Skip`, which ends the chain for later hooks (Frame Capture, other `Last` hooks).
- **Frame Capture** numbers draws differently: the probe's `#n` counts only screen-sized draws.

## Limitations

- Only draws whose render target 0 has the back-buffer size are probed. Shadow maps, bloom, reflection maps and
  half-resolution passes never appear. `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` are not hooked.
- At most 4,000 screen-sized draws per capture.
- The query wait can stall the render thread for up to 1.5 s (a visible hitch).
- The pixel is mapped by client-rectangle scaling; with the Apex menu open the cursor may be over the menu.
- The final colour is read only from 8-bit `X8R8G8B8` / `A8R8G8B8` back buffers; otherwise the file says
  `(not read)`.
- Draw pointers (`VS=`, `PS=`) are not stable between sessions; light indices from the lighting snapshot are not
  stable between dumps.
- The comparison state is lost when the game closes; the first capture after a start has no comparison.
- The one-click pointer selection (`LightProbe::Aim`, `ConfirmAim`) is implemented and handled by the overlay, but no
  menu control starts it in the current Report page; captures start from the shortcut only.
- Planned detail not implemented: replacement shader pointers for mod draws, constants c144..c149, a lamp block id
  ([history](../../history/dev-tools-light-probe.md)).

## Technical reference

### State machine

`enum class State { Idle, Armed, Capturing }` in `light_probe.cpp`, driven by `LightProbe::OnPresent(device)`, which
Night Lighting's Present hook calls every frame (`patches/night_terrain_relight_patch.cpp`, after its own `OnPresent`).

| Present | Transition | Work |
|---|---|---|
| N | Idle -> Armed | `Hotkeys::Take(Action::Probe)` (or a confirmed click): reads the back-buffer size, converts the cursor to client coordinates of `D3DDEVICE_CREATION_PARAMETERS::hFocusWindow` (or the foreground window), scales to back-buffer pixels (clamped), releases the previous draw list, registers `RegisterDrawIndexedPrimitive` / `RegisterDrawPrimitive` (name `"LightProbe"`, `Priority::Last`), posts the note "Capturing the light under the mouse…" |
| N+1 | Armed -> Capturing | the draws of the following frame are recorded |
| N+2 | Capturing -> Idle | `FinishCapture`, then the hooks are unregistered unless a texture is replaced |

### Per draw while capturing

Only when render target 0 has the back-buffer size, up to `kMaxDraws = 4000`:

- `GetTexture` s0..s15 (AddRef'd until the file is written) and MIN/MAG/MIP filter of each sampler; VS and PS.
- 14 render states (`kRecordedStates`): ZENABLE, ZWRITEENABLE, ZFUNC, ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP,
  ALPHATESTENABLE, COLORWRITEENABLE, STENCILENABLE, CULLMODE, SRGBWRITEENABLE, DEPTHBIAS, SLOPESCALEDEPTHBIAS.
- PS constants c0..c223 (`kPsConsts = 224`) and VS constants c0..c255 (`kVsConsts = 256`), non-zero registers only
  (skinned objects keep their world matrix and vertex lights at VS c184..c199), written with 9 significant digits.
- `LotLightBridge::DescribeDraw()` (the `mod:` line).
- An occlusion query (`D3DQUERYTYPE_OCCLUSION`, pooled in `g_queryPool`) around a second copy of the draw with colour
  writes off on all 4 MRT slots, Z write off, stencil write mask 0, scissor test on with a 1x1 rectangle at the pixel.
  `g_inProbeCall` stops the probe from recording its own copy. The original draw continues (`HookAction::Continue`).

Occlusion samples mean "fragments of this draw at that pixel that passed the depth, stencil and alpha tests when it ran".
A draw later overdrawn still has samples > 0. The value is the MSAA sample count (`amostras=8` with 8x MSAA).

### FinishCapture

Polls every query with `D3DGETDATA_FLUSH` in a `Sleep(1)` loop for at most 1.5 s; queries still pending are counted as
`sem resposta` and their draws dropped. Reads the final colour (`ReadScreenPixel`: `StretchRect` of the pixel to a 1x1
render target, then `GetRenderTargetData`). Writes the report, dumps shaders and textures, releases the references and
calls `Captures::Finish(folder, ..., CaptureKind::LightCapture)`. Status line:
`Measured: N draws cover pixel (x, y), M textures saved in Captures\<folder>.`, also logged as `[LightProbe] ...`.

### Automatic follow-ups after a floor change

`Watch` state: after a shortcut capture (not a click), the pixel, `ShownStories()` (from
`LevelLightShare::DisplayLevels`: the story each loaded lot shows) and a 120 s deadline are kept, with 6 captures left.
When a lot's shown story changes, captures are queued 1 s and 3 s later with the reason
`automatic, 1 s after the floor change (lot XXXXXXXX story a -> b)`; the recorder gets a `[probe] floor change` line.
Automatic captures go into `... Light capture (automatic)\` folders. Keep the camera still so the pixel stays on the same
object.

### Texture replacement (developer mode)

While any `T<n>` box is ticked the hooks stay registered. Every draw that binds a ticked texture on any sampler is
re-issued with a 1x1 managed A8R8G8B8 texture (0xFF000000 or 0xFFFFFFFF, created once), the original textures are
restored and the hook returns `Skip`. Unticking everything unregisters the hooks while Idle.

### Two-capture comparison

For each capture the probe picks one object draw among the covering draws: rank 3 if the `mod:` text starts with
`OBJECT` (an object the mod lit), rank 2 if it contains `object with rig`, rank 1 if it has more than 2 primitives, else
0; the last draw of the highest rank wins (`g_prevPick`: pixel, index, VS/PS pointers, `mod:` text, constants, screen
colour). The next capture writes a `COMPARACAO COM A CAPTURA ANTERIOR` section listing PS and VS registers whose values
differ (relative tolerance 1e-4).

### Output files

One folder per capture: `Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\<YYYY-MM-DD HH-MM-SS> Light capture\`
(or `Light capture (automatic)`; inside a session, `<session>\<HH-MM-SS> Light capture\`). See
[bug-reports.md](../bug-reports.md#technical-reference) for the files every capture gets.

| File | Content |
|---|---|
| `Light capture.txt` | the report |
| `VS_<ptr>.bin`, `PS_<ptr>.bin` | raw shader bytecode (`GetFunction`); `<ptr>` = the D3D object address |
| `VS_<ptr>.txt`, `PS_<ptr>.txt` | `D3DDisassemble` output from `d3dcompiler_47.dll` (loaded once with `LoadLibraryA`); without it the report says `.bin (no disassembler)` |
| `T<n>_<W>x<H>_<FMT>.bmp` | level 0 of each 2D texture, 32-bit BMP; RGB scaled by 1/peak when a channel exceeds 1 (FP targets) |
| `T<n>_<W>x<H>_<FMT>_alpha.bmp` | the alpha channel as grey, when it carries data (for example room light maps) |

Texture dump rules (`DumpTexture`): cube and volume textures are listed but not saved; DEFAULT-pool textures are read
only when they are render targets (`GetRenderTargetData`); DXT1/3/5 are decoded (`DecodeDxt`) when not in DEFAULT pool
and at most 2048x2048; INTZ, ATI2 and unknown formats are skipped. Texel formats: A8R8G8B8, X8R8G8B8, A16B16G16R16F,
A32B32G32R32F, R32F, R16F, G16R16F, G32R32F, L8, A8L8, A8, A16B16G16R16, A2R10G10B10, R5G6B5.

### Reading a capture

The report keeps its original Portuguese labels:

```
S3SS Light Probe | <folder> | pixel (1372, 991) | tela 3840x2160 | desenhos na tela: 446 | sem resposta: 0
Capture: requested by the shortcut; ...
Estados: z/zwrite/zfunc/blend/src/dst/blendop/atest/cw/stencil/cull/srgb/depthbias/slopebias (bits de float)

== DESENHO #13 DIP tipo=4 prims=408 amostras=1 VS=3179CDA0 PS=3179DC78
   shaders: VS VS_3179CDA0.txt | PS PS_3179DC78.txt
   estados: z=1 zwrite=1 zfunc=4 blend=0 src=2 dst=1 blendop=1 atest=0 cw=15 stencil=0 cull=2 srgb=0 depthbias(bits)=0 slopebias(bits)=0
   s0: T1 2D 256x128 A8R8G8B8 MANAGED/SYS mips=1 filtro min/mag/mip=2/2/0
   mod: game (the mod did not replace this draw) | VS other | PS other | rig mode -1
   PS c0..c223 (zeros omitidos): [0](1 1 1 0) ...
   VS c0..c255 (zeros omitidos): [0](...) ...
== COR NA TELA NO PIXEL: (0.137 0.090 0.051)
== COMPARACAO COM A CAPTURA ANTERIOR (desenho do objeto em cada pixel) ==
== TEXTURAS DOS DESENHOS QUE PINTAM O PIXEL ==
T1 2D 256x128 A8R8G8B8 MANAGED/SYS mips=1: salva T1_256x128_A8R8G8B8.bmp; media RGBA=(...) min=(...) max=(...)
```

| Field | Meaning |
|---|---|
| `tela WxH` | back-buffer size (the probe's pixel space) |
| `desenhos na tela` | screen-sized draws recorded in the frame (not only the covering ones) |
| `sem resposta` | queries still pending after 1.5 s (their draws are dropped) |
| `Capture:` | why the capture was taken (shortcut, guided click, or automatic follow-up) |
| `DESENHO #i DIP/DP` | index among recorded draws; DrawIndexedPrimitive or DrawPrimitive |
| `tipo` | `D3DPRIMITIVETYPE` (4 = triangle list, 5 = strip) |
| `amostras` | occlusion samples at the pixel (1 without MSAA, 8 with 8x MSAA) |
| `VS=/PS=` | object pointers, also the dump file names. Identify shaders by content (MD5 or FNV-1a of the bytecode) |
| `estados` | the 14 states; depth bias values are raw float bits |
| `sN: T<k> ...` | texture on sampler N; `filtro min/mag/mip` = D3DTEXF (0 none, 1 point, 2 linear, 3 anisotropic) |
| `mod:` | `LotLightBridge::DescribeDraw()` (below) |
| `PS c.. / VS c..` | non-zero float constants, 9 significant digits |
| `COR NA TELA` | final back-buffer colour at the pixel, 8-bit, or `(not read)` |
| texture line | description, dump file, mean RGBA, min RGB, max RGBA, and `(imagem escalada por 1/x)` when normalised |

### The `mod:` line

`DescribeDraw`, `DescribeObjectDraw` and the fence branch in [`lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp):

| Text | Meaning |
|---|---|
| `Night Lighting has no active hooks` | the bridge's hooks are not registered (bridge and fixes off, or hook failure) |
| `game (the mod did not replace this draw) \| VS <class> \| PS <class> \| rig mode <m>` | the game's own draw. VS classes 0..11: other, roof, lake, snowy lot, road, floor, foliage, fence/stairs (instanced), snow on object, snow with relief, object with rig, snowy floor. PS classes: unknown, other, world candidate, lot light, object rig, roof, lake, snowy lot, snowy roof, outside wall, outdoor floor. Rig mode from `RigTracker::CurrentMode()`: -1 none, 0 indoor with ceiling, 1 roofless fenced area, 2 outdoor |
| VS class 10 extras | `object: world c<K>, vertex lights c<V>` and `PS not tested yet` / `PS accepted` / `PS REFUSED (outside the pattern)`, plus `object option off`, `indoor rig (the game uses the lot light map; the mod leaves it)` or `no ground light atlas` |
| `mod draw (another fix)` or `mod draw: <fix detail>` | a draw issued by the mod itself (`g_inOwnCall`), for example `mod draw: world terrain \| ...` or `mod draw: lot light pass \| soft edges: ...` |
| `OBJECT fixed by the mod \| ...` | an outdoor object the mod redrew (filled only while capturing): PS form (A/B with rig lamps in the PS, C without), rig mode, per-pixel light on/off and strength, ground-light strength, object position (VS world triple `.w`), up to 8 per-pixel lamps (position, weight W = 0.4 x range, colour, distance, "N used of M in range"), the game's rig in PS c0..c13 and the vertex lights in VS `c(vl-4)..c(vl+3)` (kept: the patched shader takes max(rig, per-pixel)) |

### Constants that recur in captures

| Register | Meaning |
|---|---|
| VS c40..c43 | camera view-projection, identical in every scene draw; other families use c0, c4, c180, c192, c216 |
| VS c8.w / c10.w | chunk or object translation (terrain chunk centre, roof origin) |
| PS c1..c3 / c5..c7 | rig lamp directions / colours (objects); sun in c8/c9, c0/c4 or c12/c13 |
| VS c4..c11 | vertex-light directions / colours (fences, rails, stairs) |
| VS c27..c29[a0], c54..c56[a0] | per-instance lamp directions / colours (instanced foliage and fences) |
| PS c3.x / c4.x | light-map scale of lot light passes (summer / snow) |

Projection layout: row2 = A x row3 + (0, 0, 0, -near), A = 1.00008; near is about 0.2 to 0.3 and varies per frame.

### Files and functions

| File | Symbol | Role |
|---|---|---|
| `light_probe.h` | `OnPresent`, `RenderUI`, `Shutdown`, `Capturing`, `Busy`, `Aim`, `Aiming`, `CancelAim`, `ConfirmAim` | API |
| `light_probe.cpp` | `OnPresent` | shortcut, pixel mapping, state machine, follow-ups |
| | `OnDraw<DrawFn>` | per-draw recording, occlusion copy, texture replacement |
| | `RegisterHooks` / `UnregisterHooks` | draw hooks |
| | `FinishCapture`, `StartCaptureFolder` | query collection, report, pick, comparison, capture folder |
| | `DumpShader<S>`, `DumpTexture`, `DecodeDxt`, `Texel`, `HalfToFloat`, `WriteBmp` | dumps |
| | `ReadScreenPixel`, `WriteComparison`, `ShownStories` | final colour, register diff, shown stories |
| | `RenderUI` | status and replacement checkboxes (Developer page) |
| `lot_light_bridge.cpp` | `DescribeDraw`, `DescribeObjectDraw`, `g_objDrawInfo` | the `mod:` line |
| `patches/night_terrain_relight_patch.cpp` | Present hook, `RenderDeveloperUI` ("Light probe textures"), `Uninstall` (`Shutdown`) | host |
| `apex_gui.cpp` | `CaptureNote`, `CaptureTarget`, `GuiClient::OnWindowMessage` | on-screen note, aiming target, click confirmation |

No game addresses: the probe works only at the D3D9 level.

## Rejected approaches

- Static reverse engineering of lighting paths without a capture: repeatedly wrong. [History](../../history/dev-tools-light-probe.md)
- Naming shaders by pointer as an identity: pointers are reused across sessions. [History](../../history/dev-tools-light-probe.md)
- Three significant digits for constants: produced a wrong "near = 1.0" reading. [History](../../history/dev-tools-light-probe.md)
- One shared `LightProbe\` folder overwritten by every capture: mixed stale files; replaced by one folder per capture. [History](../../history/dev-tools-light-probe.md)

## See also

- [Validation](../../validation/dev-tools-light-probe.md)
- [History](../../history/dev-tools-light-probe.md) (including the index of captures behind the Night Lighting fixes)
- [Report a problem](../bug-reports.md), [Light Diag](light-diag.md), [Recorder](recorder.md), [Frame Capture](frame-capture.md)

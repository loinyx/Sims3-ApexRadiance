# Depth Blur

> Depth of field without a near field: the scene behind what the camera looks at is blurred, progressively, from the
> scene depth, and applied to the finished 3D scene **before the game draws any UI**, so pie menus, tooltips, plumbobs
> and panels stay sharp. Two focus modes: **Auto** (default since 2026-09-28: the focus follows the centre of the screen,
> computed on the GPU) and **Fixed** (the original curve: the blur starts at a set "distance"). It fades out over 0.3 s
> while the game's map view is open. The module also owns the **INTZ depth swap** that makes the game's depth buffer
> readable (`DepthShare`), which Reflections (water screen-space reflection) reads too.
> Status: flagged `experimental` in the patch registry. **The 2026-09-28 rewrite (auto focus, progressive gather, 2x2
> prep, bilateral upsample, FP16 linear light, GPU timing) is written but not compiled or tested in game yet.** Present in
> both build flavours; the Developer subsection exists only in the dev build (`kPublicBuild == false`).
> Patch name `DepthBlur`, menu page Image > Depth Blur, settings in `[patches.DepthBlur]`.

## Purpose

ReShade-style DOF blurs the UI too, because it runs on the final frame (see the original request in the old scratchpad
`s3ss_feature_request_dof.md`: ReShade CinematicDOF blurred pie menus; REST, which can inject before a draw, crashed under
DXVK). Doing it inside the D3D9 hooks lets the blur run between the last scene draw and the first UI draw. The same
insertion point and depth access later served Edge Smoothing, the (now removed) Ambient Occlusion, the HDR sky boost and
the water reflections.

## User-facing settings

All live (read every frame; `Update()` only clears `pendingReinstall`, a reinstall would tear the depth swap down from the
wrong place). Saved by the patch system into `[patches.DepthBlur]` of `ApexRadiance.toml`, plus the usual `enabled` key.
**Migration:** configs written before the rewrite have none of the new keys, so they load with the defaults, i.e. Auto
focus with Blur amount 50%; their Fixed keys (`distancia`, `transicao`, `farPlane`) are kept and used when the player
picks Fixed.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Focus: Auto / Fixed | `focoAuto` | bool | true | | Auto = the focus follows the screen centre; Fixed = the original distance curve. Auto falls back to Fixed when no 1x1 float render target exists (log line, Developer read-out). |
| Blur amount (both modes) | `quantidade` | float | 0.5 | 0 - 1 | Largest blur radius = amount x 1% of the screen height (full-res pixels): 10.8 px at 2160p, 5.4 px at 1080p for 0.5. 0 = no GPU work. |
| Sharp area (Auto) | `areaNitida` | enum int | 1 (Medium) | Small / Medium / Large | (S, T) = (1.5, 4), (2, 6), (3, 10): blur starts at S x the focus distance and is full at T x it. |
| Distance (Fixed) | `distancia` | float | 0.349 | 0.0 - 0.5 | Start of the ramp in the heuristic "linear depth" (see "Fixed focus"). Segments Near 0.25, Medium 0.349, Far 0.45; "Fine-tune distance" = the same key as 0-100%. |
| Transition (Fixed) | `transicao` | float | 0.20 | 0.01 - 0.5 | Ramp length after the start. Was under Advanced before the rewrite; now a main row in Fixed. |
| Sharp in map view | `offInMapView` | bool | true | | Fade the blur out while `MapView::IsOpen()`. Note "Map view can't be detected on this game version" when the getter was not found. |
| Advanced > Strength | `forca` | float | 1.0 | 0 - 1 | Global multiplier on the blur amount k (both modes). In Advanced because Blur amount is the main size control. |
| Advanced > Quality | `qualidade` | enum int | 2 (High) | Low / Medium / High / Ultra | Gather taps per side: 4 / 6 / 8 / 12 (9 / 13 / 17 / 25 taps per axis). |
| Advanced > Focus speed (Auto) | `velocidadeFoco` | float | 0.3 | 0.1 - 1.0 | Easing time constant tau in seconds (shown "0.3 s"). |
| Advanced > Blur the sky | `blurSky` | bool | true | | Sky pixels (d >= 0.99999) get k = 1 (true) or 0 (false). |
| Advanced > Glowing lights | `realceLuzes` | bool | true | | Near-white taps weigh a little more in blurred areas (lamps stay bright). |
| (not shown) | `tamanho` | float | 1.5 | 0.5 - 6.0 | Legacy Gaussian spread of the old blur. Still registered so old configs and profiles round-trip; **no longer used**. |
| Developer > Far plane (dev) | `farPlane` | float | 1000.0 | 10 - 10000 | Fixed only. A curve parameter of the heuristic, NOT the game's far plane. |
| Developer > Show blur amount (dev) | `debugView` | bool | false | | Composite paints k as grey (white = blurred); in Auto the focus window is tinted violet. Runs even at strength 0 / amount 0 and in map view. |

Every row passes its default (`Params{}`), so changed dots and per-row Reset work and the search finds them. Rows that
belong to one mode (Sharp area, Focus speed / Distance, Fine-tune, Transition) are drawn only in that mode, like Edge
Smoothing's FXAA-only rows, so the search finds them only while that mode is selected.

Developer section (dev build): status; focus mode (`Auto`, `Fixed`, or `Auto (no float target: using Fixed)`); in Auto
the focus depth `A - d_f` and "about N m (approx, assumes near 0.25)"; GPU cost; blurred frames, taps per side, blur
target format; map view state and fade; "Show blur amount"; Far plane.

## How it works

### Enable / disable (who runs the depth swap)

`DepthBlurPatch::Install()` sets `g.blurOn = true`, `g.focusSnap = true`, calls `UpdateDepth()` and
`PostScene::Add(PostScene::kDepthBlur, BlurEffect)`. `Uninstall()` removes the effect, sets `blurOn = false`, calls
`UpdateDepth()` again and releases the shaders (`EnsureShaders` recompiles them if the swap stayed up for a requester).

`UpdateDepth()`: the swap runs while `g.blurOn || g.requests > 0`.
- `StartDepth()` registers a registry **Present** hook (name `"DepthBlur"`, `Priority::First`) that calls
  `OnFrameBoundary`, and adds `OnPreReset` / `OnPostReset` to `RenderCallbacks::preReset` / `postReset`. Sets `g.active`.
- `StopDepth()` unregisters them and calls `ReleaseResources(ApexD3D::Device())`.
- `DepthShare::Request(bool)` increments / decrements `g.requests`. The standalone has no requester (AO and the HDR sky
  boost are removed, [../removed-features.md](../removed-features.md)), so the swap runs only while Depth Blur is on.
  Reflections never request it; they use the texture only when it exists.

### Resources (`InitResources`, at Present, between frames)

- `ExtraHooks::EnsureInstalled(dev)`; failure -> "ERROR: could not install the depth hooks".
- Backbuffer must not be multisampled, else "Edge Smoothing is on: turn it off in the game's Options > Graphics" (the
  menu shows "Paused while the game's own Edge Smoothing is on").
- The bound depth-stencil must be the backbuffer's size, no MSAA, `D3DFMT_D24S8` / `D24X8` (else "Waiting for the game
  ..."); kept (AddRef) as `g.origDS`.
- **INTZ** texture `W x H` (`D3DUSAGE_DEPTHSTENCIL`, `MAKEFOURCC('I','N','T','Z')`) and its level 0 `g.intzSurf`.
- **fullTex**: `W x H` render-target texture in the backbuffer's format (the Prep pass's colour source; 33 MB at 4K).
- **halfA / halfB**: `ceil(W/2) x ceil(H/2)`. `A16B16G16R16F` when `CheckDeviceFormat` says it is a render-target
  texture **and** filterable (`D3DUSAGE_QUERY_FILTER`) and creation succeeds; else `A8R8G8B8` with the log line
  `[DepthBlur] 16-bit float render targets not available, using A8R8G8B8 (blur in gamma space)` (then `linearLight` is
  off: an 8-bit linear image would band in the darks). Alpha = the blur amount k between passes.
- **focus[2]**: two `1x1` render targets, first of `R32F`, `R16F`, `A16B16G16R16F` that passes `CheckDeviceFormat` and
  creates; none -> `[DepthBlur] No 1x1 float render target ...: Auto focus falls back to Fixed`.
- Shaders `FocusPS`, `PrepPS`, `CompositePS` and `BlurPS` for the current quality (`TAPS` macro), ps_3_0,
  `D3DCOMPILE_OPTIMIZATION_LEVEL3`, "depth_blur.hlsl". Failure -> "ERROR: the blur shaders did not compile (see
  ApexRadiance_LOG.txt)"; not retried every frame (`fixedTried` / `blurTried`).
  **Precompiled (standalone, 2026-09-28):** the three fixed passes and all four `BlurPS` qualities are compiled at
  start-up on a background thread (`framework/shader_cache.h`, see [architecture 4.6](../architecture.md#shader-precompile));
  `EnsureShaders` / `BlurShader` only create the objects from that bytecode (`CreateShader`), also after `Uninstall`
  released them and when the quality changes. Same compile inputs, so the same bytecode. Not tested in game yet.
- 4 sets of timestamp queries (disjoint, begin, end, freq), as Edge Smoothing.
- Substitution on, `ready = true`, INTZ bound. Log: `[DepthBlur] Resources ready (WxH, INTZ depth swapped in, blur
  targets A16B16G16R16F, focus target R32F)`.
- During the frame the ExtraHooks detours swap `origDS` <-> `intzSurf` in Set/GetDepthStencilSurface; the game never
  sees the swap.

### Per frame (`BlurEffect`, PostScene order 30, after Edge Smoothing)

1. Returns if `!blurOn || !ready || inBlur || internalPass`.
2. `StepTime()`: `dt` from QPC since the previous call, clamped to 0.1 s. `StepMapFade(dt)`: `mapOpen = offInMapView &&
   MapView::IsOpen()`; the frame it turns false sets `focusSnap`; `mapFade` moves toward 1 / 0 by `dt / 0.3 s`.
3. No GPU work when not in debug view and `strength x (1 - mapFade) <= 0` or `amount <= 0`.
4. Timestamp queries around `RunBlur` (read back a few frames later, never waited on; EMA 0.9 / 0.1; restarted when
   Quality changes). `GpuCostMs()` returns it, so the card header and the Overview row show the "~x ms" chip.
5. `RunBlur(dev, dt)`:
   1. `StretchRect(backbuffer -> fullTex, D3DTEXF_NONE)`.
   2. `SavedState::Capture` (RT0, depth-stencil, PS, VS, declaration, FVF, stream 0, textures and 8 sampler states of
      **s0..s3**, 15 render states, PS constants **c0..c6**, viewport). No state block (CPU heavy).
   3. Unbind the depth-stencil (the INTZ is sampled), `SetPassStates` (Z / blend / stencil / fog / sRGB off, cull none;
      all 4 samplers POINT + CLAMP, no mips).
   4. Constants (below), INTZ on s2.
   5. **Focus** (Auto only): RT = `focus[1 - cur]`, s3 = `focus[cur]`, 1x1 quad; `cur` flips; `focusSnap` clears. Then
      s3 = the new focus for the next passes (null in Fixed).
   6. **Prep**: `fullTex` (s0, point) -> halfA.
   7. **Gather H** halfA -> halfB, **gather V** halfB -> halfA (s0 LINEAR).
   8. **Composite** into the backbuffer: halfA on s1 (point), alpha blending SRCALPHA / INVSRCALPHA, colour write RGB
      only (the backbuffer alpha stays the game's).
   9. `SavedState::Restore` (RT0 first because SetRenderTarget resets the viewport; stream 0 because DrawPrimitiveUP
      clears it; the depth-stencil with `RawSetDepthStencilSurface`, i.e. the INTZ again).
6. **Reset**: `OnPreReset` -> `ReleaseResources` (clears the substitution, re-binds `origDS`, releases every default-pool
   resource, queries, the read-out surface; sets `focusSnap`). `OnPostReset` sets `retryCountdown = 0`.

### Constants

| Reg | x | y | z | w |
|---|---|---|---|---|
| c0 `cParams` | start (Fixed) | range (Fixed) | strength x (1 - mapFade) | farPlane (Fixed) |
| c1 `cHalf` | half W | half H | 1 / half W | 1 / half H |
| c2 `cDir` | gather dir x | gather dir y | max radius R (half-res px) = amount x 0.01 x H / 2 | lamp weight (2 or 0) |
| c3 `cFlags` | blur sky | debug view | linear light | auto focus |
| c4 `cFull` | W | H | 1 / W | 1 / H |
| c5 `cFocus` | c0 = 1 - 1/S | 1 / (c1 - c0), c1 = 1 - 1/T | A = 1.00008 | - |
| c6 `cEase` | ease = 1 - exp(-dt / tau) (1 on snap) | 1 = ignore the previous focus | window half width (uv) = 0.025 H / W | window half height (uv) = 0.025 |

### Auto focus (`FocusPS`, GPU only, no CPU readback)

- 16 depths on a 4 x 4 grid spanning a centred square window of 5% of the screen height (`kFocusWindow`), each snapped to
  a full-res texel centre and point-sampled; sky (d >= 0.99999) is replaced by 2 and never counts.
- Statistic: the **25th percentile** of the n non-sky samples = the t-th smallest with `t = max(1, ceil(n / 4))`,
  computed without sorting: a sample x is a candidate when at least t samples are <= x (4 `step` + 1 `dot` per sample);
  the smallest candidate wins. Why the 25th percentile rather than the median: the nearer quarter wins, so a Sim or
  object covering a quarter of the window holds the focus against the background behind it (the median needs half);
  a thin post or leaf covering one or two samples still cannot grab it; and it cannot be dragged far by one outlier.
- Stored value `F = A - d_f` (proportional to 1/z_f; the near plane cancels in the ratio below).
  `next = prev > 0 ? lerp(prev, F_target, ease) : F_target`, with `ease = 1 - exp(-dt / tau)` from the CPU frame time.
  Easing in 1/z space is what a lens does (blur is linear in 1/z).
- All samples sky: keep `prev`. Snap (ignore `prev`): first frame after `InitResources` (so after every device Reset),
  when the feature is installed, when the map view closes, and when the mode switches to Auto. A snap with only sky
  stores -1 = "unknown": then nothing but the sky blurs until something is seen.
- Two 1x1 targets ping-pong (read `focus[cur]`, write `focus[1 - cur]`); Prep and Composite read the new one with one
  point fetch per pixel.

### Blur amount per pixel (`BlurAmount`)

Auto (thin-lens ratio; the game's projection is `d = A - near*A/z`, A = 1.00008, near 0.2 - 0.3):
```
r = (A - d) / (A - d_f) = z_f / z          // near cancels
c = 1 - r                                  // 0 at the focus, -> 1 at infinity, < 0 nearer than the focus
k = saturate((c - c0) / (c1 - c0)),  c0 = 1 - 1/S,  c1 = 1 - 1/T
nearer than the focus (c < 0): k = 0 (no near-field blur in this version)
focus unknown (stored value <= 0): k = 0
```
| Sharp area | S, T | c0, c1 | blur starts / is full at |
|---|---|---|---|
| Small | 1.5, 4 | 0.333, 0.75 | 1.5 x / 4 x the focus distance |
| Medium (default) | 2, 6 | 0.5, 0.833 | 2 x / 6 x |
| Large | 3, 10 | 0.667, 0.9 | 3 x / 10 x |

Fixed (the original heuristic, unchanged):
```
lin = d / (F - d * (F - 1))                 // F = farPlane (1000)
k   = saturate((lin - start) / max(range, 1e-4))
```
Both: sky (d >= 0.99999) -> k = blurSky; then `k *= strength x (1 - mapFade)`. Per-pixel blur radius `r_px = k x R`
(half-res pixels), R = amount x 1% of the screen height / 2, so the look is the same at any resolution (the old blur's
spacing was in half-res pixels, so 4K got half the visible blur of 1080p).

Fixed "distance" in metres (computed with near 0.25, F = 1000; the same setting moves 20% with near 0.2 / 0.3, i.e.
with zoom): `z = near * (1 + lin*(F-1)) / (1 - lin)` -> Near 0.25 about 84 m, Medium 0.349 about 134 m, Far 0.45 about
205 m, default full blur (0.549) about 305 m.

### Prep (`PrepPS`, one half-res texel per 2x2 full-res block)

- Block top-left = `floor(uv * halfSize) * 2 + 0.5` (full-res texel centres; no reliance on where the half-res centre
  falls, the AO pitfall "half resolution picks 1 of the 4 depth pixels").
- 4 point depths -> 4 k values; 4 colours -> linear with `pow(c, 2.2)` (FP16 targets only).
- Block amount `kmin = min(k0..k3)`: a block touching a sharp edge counts as sharp, so the gather (which only accepts
  taps whose own radius reaches the pixel) never spreads it into the blurred background.
- Block colour: weights `w_i = saturate(1 - (k_i - kmin) x max(R, 2))`, i.e. the pixels whose radius is within about one
  half-res pixel of the block's; the sharpest pixel always has weight 1. So colour and amount describe the same
  surface; a uniform block is an exact 2x2 box (stable under sub-pixel motion).

### Gather (`BlurPS`, separable, half resolution)

```
r = centre.a * R                      // half-res pixels
r < 0.05: output = centre             // sharp pixels cost one fetch
taps i = -TAPS..TAPS at offset (i/TAPS) * r along the axis (bilinear)
w_i  = exp(-2 (i/TAPS)^2)             // Gaussian, sigma = r/2
w_i *= saturate(tap.a * R - |i/TAPS * r| + 1)          // scatter-as-gather acceptance (per axis)
w_i *= 1 + lampWeight * centre.a * max(luma_lin(tap) - 0.8, 0)   // Glowing lights, at most 1.4x
output = (sum w_i tap.rgb / sum w_i, centre.a)
```
H then V: the V pass uses the same per-pixel radius (alpha is carried through). The radius grows with k, so the blur
grows smoothly from sharp instead of cross-fading a fixed blur (no sharp + ghost double image). No per-frame noise, no
rotation, no temporal accumulation: identical input gives identical output.

### Composite (`CompositePS`, full resolution, alpha blended)

```
k (full-res pixel, point depth), r = k * R
alpha = smoothstep(0.1, 1.0, r)         // r in half-res px: fully blurred from 2 full-res px of radius
alpha == 0: output alpha 0 -> the original pixel exactly
4 nearest half-res texels (point, texel centres), bilinear weights bw_i
dr_i = |k_i - k| * max(R, 2)            // radius difference, half-res px
w_i  = bw_i * exp2(-2 dr_i^2)
colour = sum w_i q_i / sum w_i -> pow(1/2.2) (FP16 only)
alpha *= saturate(sum w_i * 20)         // no similar texel around: fall back to the original
debug: (k, k, k, 1), focus window tinted violet in Auto
```
Blending original (gamma) with the re-encoded blur is in gamma space; the blur itself is linear.

### Cost (estimated, not measured)

At 3840 x 2160 on an RTX 4070 Ti SUPER, Quality High: full-res copy about 0.1 ms, Prep about 0.15 ms (8 fetches per
half-res texel, 66 MB read), gather H + V about 0.2 - 0.3 ms (2 x 17 bilinear FP16 taps on 2 Mpx, sharp pixels exit
early), composite about 0.2 ms (6 point fetches per 8.3 Mpx + blend), focus pass negligible: **about 0.6 - 0.8 ms**
(Ultra about 0.9 ms). The menu chip shows the measured value. Memory: INTZ 33 MB + full copy 33 MB + 2 x 16.6 MB FP16.

### Shaders (embedded HLSL, `kShaderSource`, ps_3_0)

| Entry | Inputs | Output (target, format) | Estimated slots |
|---|---|---|---|
| `FocusPS` | s2 INTZ, s3 previous focus, c4-c6 | focus[next] 1x1 R32F (or R16F / A16B16G16R16F) | about 350 (16 fetches) |
| `PrepPS` | s0 fullTex (point), s2 INTZ, s3 focus, c0-c5 | halfA, A16B16G16R16F (or A8R8G8B8): rgb linear colour, a = kmin | about 120 |
| `BlurPS` (`TAPS` 4/6/8/12) | s0 half target (linear), c1-c2 | the other half target: rgb, a = centre k | about 300 at Ultra |
| `CompositePS` | s1 halfA (point), s2 INTZ, s3 focus, c0-c6 | backbuffer RGB, alpha-blended | about 70 |

Slot counts are estimates (not compiled here); all stay under the ps_3_0 minimum of 512 slots. Every fetch is
`tex2Dlod` (they sit in dynamic branches).

### Developer focus read-out

Dev page only: while `RenderDeveloperUI` is drawn it asks for a read-out for the next 0.5 s; `RunBlur` then copies the new
focus value to a 1x1 system-memory surface (`GetRenderTargetData`, at most 4 times a second) and issues an event
query; a later frame reads it only when the query is done (never waits). The effect itself never reads it back; the
public build never calls it. The metre value assumes near 0.25 (`z = 0.25 x A / (A - d_f)`), so it is approximate.

### Map view detection (map_view.cpp)

The "Sharp in map view" option needs to know whether the game's map view (M key, or zooming all the way out) is open.
There is no D3D-level signal for it, so `MapView::IsOpen()` calls the game's own script binding:

1. `Resolve()` (once, `std::call_once`, on the first `Available()` / `IsOpen()` call, i.e. from the render thread at the
   first PostScene trigger): find the sections `.text`, `.rdata`, `.data` of `TS3W.exe` from the PE headers.
2. Search `.rdata` for the exact string `"ScriptCore.CameraController::Camera_IsMapViewModeEnabled"` including its
   terminator (1.67.2: at `0x010000A4`).
3. The script API registers native calls from a table of `{function pointer, name pointer}` pairs. Search `.data`
   (4-byte aligned; fallback `.rdata`) for the name's address; the dword **before** it is the function pointer. On
   1.67.2 the entry is at **`.data 0x0115DD40` (function) / `0x0115DD44` (name)**. (The code comment in
   `map_view.cpp` says "0x0115DD20"; that is a neighbouring entry, `{0x0073D620,
   "ScriptCore.CameraController::Camera_SetMotion"}`. The search does not use the constant, so the code is unaffected.)
4. Validate before trusting: the pointer must lie in `.text` with 0x60 bytes to spare, start with `E8` (call), push the
   cast id `68 89 DD 0F 11` (`push 110FDD89h`) and end in `8A 80 xx xx xx xx C3 32 C0 C3`
   (`mov al, [eax+disp32]; ret; xor al, al; ret`) within the first 0x50 bytes (`LooksLikeGetter`). Otherwise log
   `[MapView] Unexpected function at 0x..., map view detection off`.
5. Found: log `[MapView] Camera_IsMapViewModeEnabled at 0x0073e060`.
6. `IsOpen()` calls it as `bool __cdecl()` inside `__try/__except`; a fault logs `[MapView] Call faulted, map view
   detection off` and disables detection for the session.

What the function does (static disassembly, `engine_map\full.asm`, verified): `0x0073E060` calls `0x0096B390` (app),
`0x00F20C20` (world), `0x0096B6D0` (camera manager), then the manager's vfunc `+0x0C` with cast id `0x110FDD89` (camera
interface), and returns the byte at **camera + 0x8B9**, or 0 when any link is null. It only reads, so calling it from
the render thread is safe. Details of the camera object: [../engine/camera-and-map-view.md](../engine/camera-and-map-view.md).

The fade (`StepMapFade`) is a linear ramp of `kMapFadeSeconds = 0.3` s on `mapFade`, applied as
`k x strength x (1 - mapFade)`, so the blur radius shrinks to zero; at `mapFade = 1` Depth Blur does no GPU work.

### Standalone baseline

The standalone took Depth Blur from the v0.1.0 commit `b84d5f1` plus the later SMAA and map-view fade; the 2026-09-28
rewrite above replaced the blur itself. The standalone's `post_scene.cpp` is the v0.1.0 one: **it has no camera votes**
(`CameraNear`, `CameraViewProj`, `CameraDepthA` existed only in the combined build, for Ambient Occlusion). Depth Blur
does not need them: the Auto focus works with depth ratios, where the near plane cancels. The standalone Frame Profiler
has no `CameraViewProj` fallback either (frame_profiler.cpp: frames without the lot LOD call count as "unknown").

## Shared machinery (post-scene chain, depth share, extra hooks)

Also summarised in [../architecture.md](../architecture.md); the details that matter for this feature:

### PostScene (post_scene.cpp / post_scene.h)

- **Chain in the standalone:** `kEdgeSmoothing = 20`, then `kDepthBlur = 30` (10 was the removed Ambient Occlusion).
  Effects are kept in a vector sorted with `std::stable_sort` by order; `Add` ignores duplicates.
- `PostScene::Add` registers the registry hooks (name `"PostScene"`, all `Priority::First`) with the first effect;
  `Remove` unregisters them with the last one. On first registration `g_done = true`, so nothing runs until the next
  Present.
  - Present -> `OnFrameBoundary`: refresh the backbuffer identity, clear `g_sceneDraws` and `g_done`.
  - SetRenderTarget -> track RT0 identity (`index == 0`).
  - DrawIndexedPrimitive and DrawPrimitive -> `OnGameDraw`.
- **`OnGameDraw`, the "scene done, before UI" detector:**
  - ignored when `g_done` or `DepthShare::InternalPass()`; ignored unless RT0 is the backbuffer;
  - `D3DRS_ZENABLE != D3DZB_FALSE` -> a scene draw: `g_sceneDraws++`;
  - `ZENABLE == FALSE` with `g_sceneDraws >= 20` (`kMinSceneDraws`) -> the trigger: `g_done = true` first, copy the
    effect list under the mutex, run each effect. The effects run inside the game's draw call, before it executes.
  - The first depth-off backbuffer draw after the scene is the **bloom composite** when bloom is on, else the first UI
    draw, so the effects run **before the bloom is composited**.
- Effects draw with `DrawPrimitiveUP`, which is not a registry hook, so they never re-trigger PostScene. Their
  `SetRenderTarget` calls go through the registry (recursive mutex): PostScene's RT0 tracker follows them, and each
  effect restores RT0 at the end.
- lot_light_bridge's DIP/DP hooks (`Priority::Normal`) return Skip for draws they replace; PostScene is at
  `Priority::First`, so it sees every game draw first. Its extra draws (lake/water pass) are wrapped in
  `DepthShare::SetInternalPass(true/false)` because that pass turns ZENABLE off and unbinds the depth-stencil, which
  would otherwise look like the first UI draw (25/09 adversarial review, item 1).

### DepthShare (depth_share.h, implemented at the end of depth_blur_patch.cpp)

| Function | Returns / does |
|---|---|
| `Texture()` | `g.intzTex` when `ready`, else null |
| `Surface()` | `g.intzSurf` when `ready` (level 0, the surface bound as depth-stencil while the scene renders) |
| `SetInternalPass(bool)` / `InternalPass()` | the flag above (render thread only, plain bool) |
| `Request(bool)` | reference-counted request to keep the swap running with Depth Blur off |
| `Status()` | Depth Blur's status string |

Consumers must check that `Surface()` is the depth-stencil bound right now (via `ExtraHooks::RawGetDepthStencilSurface`)
before trusting the texture as the main scene's depth, and must unbind it (`RawSetDepthStencilSurface(nullptr)`) while
sampling it, restoring after.

### ExtraHooks (d3d9_extra_hooks.cpp / .h)

- Detours on device vtable slots not covered by the registry: 34 `StretchRect`, 39 `SetDepthStencilSurface`,
  40 `GetDepthStencilSurface`, 43 `Clear`, 83 `DrawPrimitiveUP`, 84 `DrawIndexedPrimitiveUP`.
- Installed once, on first `EnsureInstalled(dev)` (Depth Blur's `InitResources`, or Frame Capture), never removed; with
  no callback set each costs an atomic load.
- Guard: refuses to install if any of its slots shares code with a slot already detoured by the registry (protects
  against DXVK folding functions), log `[ExtraHooks] vtable[m] shares code with vtable[o], not installing`.
- Single-owner depth substitution (`SetDepthSubstitution(substitute, report)`), observer slots used only by Frame
  Capture, and `RawSet/RawGetDepthStencilSurface` that bypass the substitution.
- Official S3SS hooks neither slot 39 nor 40, so the swap has no competitor; S3SS's ImGui uses state blocks, which do
  not include the depth-stencil binding.

### RenderCallbacks (render_callbacks.h)

Three arrays of 4 atomic slots (`endSceneBeforeOverlay`, `preReset`, `postReset`). `Add` silently does nothing when all
4 slots are taken. The standalone (no AO) has exactly four `preReset` users: Depth Blur, Edge Smoothing, Lot Map Probe,
Night Lighting's `LightmapSmooth::OnPreReset`.

## Files and functions

| File | Function / symbol | Role |
|---|---|---|
| patches/depth_blur_patch.cpp | `DepthBlurPatch` (Install / Uninstall / RenderCustomUI / RenderDeveloperUI / GpuCostMs), `APEX_REGISTER_FEATURE` | patch, settings, UI |
| | `StartDepth`, `StopDepth`, `UpdateDepth` | depth swap lifetime |
| | `InitResources`, `ReleaseResources`, `FormatSupported`, `SubstituteDS`, `ReportDS` | resources, INTZ swap |
| | `CompileShader`, `BlurShader`, `EnsureShaders`, `ReleaseShaders` | shaders |
| | `BlurEffect`, `StepTime`, `StepMapFade`, `RunBlur`, `ReadTimings`, `SavedState`, `SetPassStates` | the effect |
| | `CollectFocusReadout`, `IssueFocusReadout`, `HalfToFloat` | dev read-out |
| | `OnFrameBoundary`, `OnPreReset`, `OnPostReset` | Present / Reset |
| | `namespace DepthShare { ... }` | implementation of depth_share.h |
| features/depth_share.h | `DepthShare::*` | interface |
| features/post_scene.cpp/.h | `PostScene::Add/Remove`, `OnGameDraw`, `OnFrameBoundary` | trigger chain |
| framework/d3d9_extra_hooks.cpp/.h | `ExtraHooks::*` | depth-stencil detours |
| framework/render_callbacks.h | `RenderCallbacks::preReset/postReset` | Reset slots |
| features/map_view.cpp/.h | `MapView::IsOpen/Available` | map view flag |
| apex_gui.cpp | `DepthBlurContent` (note about the game's Edge Smoothing + the feature card), Developer `DevCard("DebugBlur", ...)` | UI placement |

## Game addresses and patterns

Depth Blur itself patches no game code. It depends on:

| Address | What | How found / verified |
|---|---|---|
| `0x0073E060` | `ScriptCore.CameraController::Camera_IsMapViewModeEnabled` (returns camera + 0x8B9) | found at run time through the name string and the binding table (see "Map view detection"); verified statically and in the log. |
| device vtable 34/39/40/43/83/84 | D3D9 methods (DXVK `d3d9.dll`) | ExtraHooks, guard above |
| projection `d = A - near*A/z`, A = 1.00008 | used by Auto focus (A only; near cancels) | LightProbe-m80 ([../engine/camera-and-map-view.md](../engine/camera-and-map-view.md)) |

## Interactions

- **Edge Smoothing (order 20)** runs before Depth Blur on the same trigger, so the blur works on the anti-aliased image.
  Both need the game's MSAA ("Edge Smoothing" in Options > Graphics) off.
- **Reflections** (lot_light_bridge.cpp water pass): screen-space ray march against `DepthShare::Texture()` when it
  exists and `Surface()` is bound; otherwise lamps only. So scenery reflections on ponds need Depth Blur on.
- **Picture filters** (picture.cpp): its scene copy is taken in draw hooks registered after PostScene's (picture.cpp:
  "After PostScene (Priority::First = 0), before Early (25)"), so the copy already contains the blur.
- **S3SS overlay:** drawn in S3SS's EndScene, after the trigger, so it is never blurred.

## Known limitations

- Needs the game's multisampling off, and the auto depth-stencil must be D24S8/D24X8 at screen size.
- No near-field blur: everything nearer than the focus stays sharp.
- Auto focus looks only at the screen centre (5% window); the game's camera usually centres the active Sim or the
  clicked spot, but a camera aimed at open sky keeps the previous focus.
- The 2x2 prep takes the sharpest pixel of each block, so the blurred background loses up to one half-res pixel along
  sharp silhouettes; the bilateral upsample then uses the other neighbouring texels, or the original pixel.
- Bilinear gather taps interpolate k across an edge, so up to about one half-res pixel of foreground colour can still
  reach the background right at a silhouette (much less than the old non-depth-aware upsample).
- Colour is blurred at half resolution; blending with the original is in gamma space.
- Trigger position: the effect runs before the bloom composite. In interiors the game has depth-off backbuffer draws
  in the middle of the scene, so in some interiors the blur may run before the scene is complete (inferred from the HDR
  diagnostic of 28/09; not observed for Depth Blur).
- Map view detection needs `Camera_IsMapViewModeEnabled`; on other game versions the option does nothing.

## Pitfalls and failed approaches

- **Lake pass mid-frame trigger** (25/09 review): any extra pass that turns ZENABLE off on the backbuffer must be
  wrapped in `DepthShare::SetInternalPass`.
- **Game MSAA on**: Depth Blur cannot share the depth; with no depth, reflections use lamps only.
- **Half-res depth from one point sample** (the AO "micro dots" root cause, [../removed-features.md](../removed-features.md)):
  a half-res pixel that point-samples one of the 4 depth pixels picks another after a 1-pixel camera move. The Prep
  pass reads all 4 at exact texel centres and combines them (min + matching colour) for that reason. Do not go back to
  `StretchRect` + one depth sample.
- **Per-frame noise / rotation / temporal accumulation**: every AO attempt with them twinkled (leaves, disocclusion).
  The blur is deterministic on purpose; keep it that way.
- **Spreading a fixed kernel's taps apart** makes dotted artefacts; the gather's tap spacing is r / TAPS (at most about
  1.4 half-res pixels at High with Blur amount 100% at 4K), and larger radii should get more taps, not wider spacing.
- **Cross-fading a fixed blur** by the mask (the old composite) shows a sharp image plus a ghost in the partial zone; the
  blur radius must grow with k instead.
- **Full state blocks** were avoided on purpose (CPU heavy); save exactly the states touched (now s0..s3 and c0..c6),
  and remember `SetRenderTarget` resets the viewport and `DrawPrimitiveUP` clears stream 0.
- **Do not call `Uninstall`/reinstall from `Update()`**: it would tear down the swap from the wrong thread.
- **Projection model:** A = 1.00008 and near 0.2 - 0.3 (27/09-28/09); early notes' A = 1 and near 1.0 / 10 were 3-digit
  rounding. `farPlane` = 1000 is unrelated to the game's far plane.
- **Held references across Reset:** `origDS` is AddRef'd; it must be released in `preReset` (done in `ReleaseResources`).
- **CPU readback of the focus** would stall the pipeline; the focus lives on the GPU. The dev read-out uses an event
  query and never waits.

## What is verified and what is not

- Verified before the rewrite (in game): the INTZ swap, the PostScene insertion point, the map view getter and fade,
  the Fixed curve's look, "Paused while the game's own Edge Smoothing is on".
- Verified in captures: `d = A - near*A/z`, A = 1.00008, near 0.2 - 0.3 (LightProbe-m80).
- **Not verified (written 2026-09-28, not compiled here):** that the HLSL compiles within ps_3_0 limits (slot counts are
  estimates); `A16B16G16R16F` render target + filtering and `R32F` 1x1 render targets under DXVK 3.1.1 (expected: DXVK
  reports both; the code checks with `CheckDeviceFormat` and falls back); `GetRenderTargetData` from a float 1x1 target
  into a system-memory surface under DXVK (dev read-out only); the look of Auto focus, the percentile choice, the
  sharp-area presets, the 0.5 default amount (at 2160p it gives a radius close to the old default's size, at 1080p
  about half the old size, computed not observed), the lamp weight 2, the composite's smoothstep(0.1, 1.0) and the
  similarity falloff; the GPU cost.

## Testing in game

- Depth Blur page: status note absent (no error); the card chip shows "~x ms" after a few frames.
- Developer (dev build): "Focus: Auto", focus depth with a plausible metre value (a Sim at the centre a few metres
  away); move the camera: the value eases in about 0.3 s; point at the sky: it keeps the last value.
- "Show blur amount": black around the centre object, white far away and on the sky; the violet square is the focus
  window. Switch Sharp area Small / Medium / Large and see the black zone grow.
- Blur amount 0% -> 100%: the background softens progressively; no double image in the transition zone.
- Pan the camera slowly: silhouettes against a blurred background must not shimmer or crawl.
- Lamps at night in the blurred background stay bright blobs (Glowing lights on) and there is no banding in dark skies.
- Pie menu over a blurred background: the menu stays sharp.
- Press M: fade to 1.00 within 0.3 s, blur gone; closing it focuses at once on the new view.
- Change resolution / alt-tab (Reset): "Recreating after a video change...", then "Active"; the focus snaps.
- Focus: Fixed shows the old distance behaviour with the new blur.
- Log: `[DepthBlur] Installed`, `[ExtraHooks] Installed (...)`, `[DepthBlur] Resources ready (3840x2160, INTZ depth
  swapped in, blur targets A16B16G16R16F, focus target R32F)`, `[MapView] Camera_IsMapViewModeEnabled at 0x0073e060`.
  Any `Shader ... failed to compile` line is a build problem of the embedded HLSL.

## Open items

- Near-field blur (in front of the focus) with its own gather (foreground spreading over the background).
- A quarter-resolution level if players want radii well beyond 1% of the screen height.
- Decide whether PostScene should fire at the last depth-tested -> depth-off transition like the Picture scene copy.
- Raise `RenderCallbacks::kSlots` or log when `Add` finds no free slot.

## Investigation: distant foliage speckles (2026-10-02)

The user supplied an image with small sharp dark/light points inside blurred trees. A candidate mechanism is the bilateral composite fallback: PrepPS stores the minimum blur amount of each 2x2 block, while CompositePS compares it to full-resolution depth. Thin alpha-tested foliage can produce mismatched blur values; low `wsum` reduces composite alpha and exposes the original pixel. Sparse gather taps are another possible source documented above. This is a code-based hypothesis, not a confirmed diagnosis. No blur shader or visual parameter was changed. Validate using the depth debug view and the same camera with quality levels before changing the fallback, which also protects foreground edges.

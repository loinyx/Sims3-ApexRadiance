# Depth Blur

Depth Blur softens the distant background, like a camera lens focused on the scene in front of it. The blur grows
smoothly with distance, lamps in the blurred background stay bright, and pie menus, tooltips, plumbobs and panels stay
sharp because the blur is applied before the game draws its interface. It fades out while the map view is open and
pauses during loading screens. The feature also owns the shared depth buffer that Ambient Occlusion, Edge Smoothing and
Water Reflections read.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (present in the first version in this repository). Loaded-world and loading-window guard: in development (PR #2) |
| Default | On for new configurations; Fixed focus |
| Menu | Image > Depth Blur; Overview > Image > Depth Blur |
| Configuration | `[patches.DepthBlur]` in `ApexRadiance.toml` |
| Source | [`patches/depth_blur_patch.cpp`](../../patches/depth_blur_patch.cpp), [`features/post_scene.cpp`](../../features/post_scene.cpp), [`features/world_session.h`](../../features/world_session.h), [`features/map_view.cpp`](../../features/map_view.cpp), [`framework/d3d9_extra_hooks.cpp`](../../framework/d3d9_extra_hooks.cpp) |

## The problem

The Sims 3 has no depth of field. A depth-of-field filter injected on the final frame (ReShade CinematicDOF, for
example) also blurs the pie menus and panels, because it cannot tell the scene from the interface. Tools that can inject
before a specific draw were unstable under DXVK. The game's depth buffer is also a plain D24S8 surface that a shader
cannot read.

## How Apex Radiance solves it

Apex Radiance swaps the game's depth buffer for a readable INTZ depth texture without the game noticing, then blurs the
scene between the last 3D draw and the first interface draw. The blur is computed at half resolution in linear light,
with a per-pixel radius that grows with distance from the focus, and blended back over the full-resolution image.

1. **Depth swap.** The game's auto depth-stencil is replaced by an INTZ texture of the same size through detours on
   `SetDepthStencilSurface` / `GetDepthStencilSurface`. The game still sees its own surface.
2. **Trigger.** The shared post-scene trigger fires once per frame when the scene is finished (see
   [Post-scene trigger](#post-scene-trigger)).
3. **Focus.** *Auto*: the GPU picks the focus distance from 16 depth samples at the centre of the screen and eases
   toward it. *Fixed*: the blur starts at a set distance on a fixed curve.
4. **Prep.** Each 2x2 block of the scene becomes one half-resolution texel holding its colour (linear light) and blur
   amount.
5. **Gather.** A separable blur whose radius per pixel equals the blur amount times the maximum radius.
6. **Composite.** A depth-aware upsample alpha-blended over the back buffer. Sharp pixels keep their exact original
   colour.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Depth Blur (card switch) | `enabled` | bool | on | | Turns the effect on |
| Focus | `focoAuto` | bool | Fixed (`false`) | Auto, Fixed | Auto keeps what the camera looks at sharp; Fixed starts the blur at a set distance. Auto falls back to Fixed when no 1x1 float render target exists |
| Blur amount | `quantidade` | float | 100% | 0 to 100% | Largest blur radius = amount x 1% of the screen height (21.6 px at 2160p, 10.8 px at 1080p at 100%). 0% does no GPU work |
| Sharp area (Auto) | `areaNitida` | enum | Large (2) | Small, Medium, Large | Blur starts at S and is full at T times the focus distance: (1.5, 4), (2, 6), (3, 10) |
| Distance (Fixed) | `distancia` | float | 0.318 | Near 0.25, Medium 0.349, Far 0.45 | Start of the ramp on the Fixed curve. The default matches none of the three steps |
| Fine-tune distance (Fixed) | `distancia` | float | 64% (0.318) | 0 to 100% (stored 0 to 0.5) | The same key as Distance, as a slider |
| Transition (Fixed) | `transicao` | float | 58% (0.288) | 2 to 100% (stored 0.01 to 0.5) | Ramp length after the start; lower gives a sharper line |
| Sharp in map view | `offInMapView` | bool | on | | Fades the blur out over 0.3 s while the map view is open |
| Advanced > Strength | `forca` | float | 100% | 0 to 100% | Multiplies the blur amount everywhere (both modes) |
| Advanced > Quality | `qualidade` | enum | Medium (1) | Low, Medium, High, Ultra | Gather taps per side: 4, 6, 8, 12 (9, 13, 17, 25 per axis) |
| Advanced > Focus speed (Auto) | `velocidadeFoco` | float | 0.1 s | 0.1 to 1.0 s | Easing time constant of the focus |
| Advanced > Blur the sky | `blurSky` | bool | on | | Sky pixels get full blur (on) or none (off) |
| Advanced > Glowing lights | `realceLuzes` | bool | on | | Near-white taps weigh more in blurred areas, so lamps stay bright (at most 1.4x) |
| (not shown) | `tamanho` | float | 0.8 | 0.5 to 6.0 | Legacy spread of the old blur. Still registered so old configurations and profiles round-trip; not used |
| Developer > Far plane | `farPlane` | float | 1000 | 10 to 10000 | Fixed focus only: a parameter of the heuristic curve, not the game's far plane |
| Developer > Show blur amount | `debugView` | bool | off | | Paints the blur amount in grey (white = blurred); the Auto focus window is tinted violet. Runs even at amount 0 and in map view (developer mode only) |

All settings are read live every frame. Rows that belong to one focus mode are drawn only in that mode, so the menu
search finds them only then. Configurations written before the Auto focus existed keep their Fixed keys and gain the new
keys with their defaults.

The tooltips of three segmented rows still describe older defaults: *Medium* is labelled "The default" for Sharp area,
*High* for Quality and *Medium* for Distance, while the registered defaults are Large, Medium and 0.318.

## Compatibility and interactions

- **The game's Edge Smoothing (MSAA)** must be off: a multisampled depth buffer cannot be sampled in Direct3D 9. The
  status reads "Edge Smoothing is on: turn it off in the game's Options > Graphics", the Overview row shows *Waiting for
  game settings*, and the shared amber compatibility card appears (see
  [edge-smoothing.md](edge-smoothing.md#compatibility-and-interactions)).
- **Post-scene order:** Ambient Occlusion (10), Edge Smoothing (20), then Depth Blur (30), so the blur works on the
  shaded, anti-aliased image.
- **Depth share consumers:** Ambient Occlusion and Edge Smoothing (*Edges from depth*) request the depth swap, so it
  keeps running with Depth Blur off. Water Reflections uses it when it exists but never requests it; its card asks for
  Depth Blur ([reflections.md](reflections.md)).
- **Picture filters** copy the scene after the post-scene effects, so the blur is graded with the scene.
- **Hidden game UI:** with no depth-off interface draw after the scene, the chain runs at the end of the frame instead.
- **Loading screens:** the blur pauses while no world is loaded or the loading window is attached; see
  [Loaded-world guard](#loaded-world-guard).
- **Compare with the game** (shortcut) turns Depth Blur off with the other effects until it is pressed again.
- **Filtered screenshots** include the blur; see [bug-reports.md](bug-reports.md#screenshot-settings).
- **Sims3SettingsSetter** hooks neither depth-stencil method, so the swap has no competitor; its ImGui state blocks do
  not include the depth-stencil binding.
- **Requirements:** INTZ depth textures; FP16 (`A16B16G16R16F`) filterable render targets for linear-light blur
  (otherwise 8-bit gamma-space blur); a 1x1 float render target (R32F, R16F or `A16B16G16R16F`) for Auto focus.

## Limitations

- No near-field blur: everything nearer than the focus stays sharp.
- Auto focus looks only at the centre (a square of 5% of the screen height). Aimed at open sky, it keeps the previous
  focus.
- The 2x2 prep keeps the sharpest pixel of each block, so the blurred background loses up to one half-resolution pixel
  along sharp silhouettes. Bilinear gather taps can let up to about one half-resolution pixel of foreground colour reach
  the background at a silhouette.
- Colour is blurred at half resolution; blending with the original is in gamma space.
- The blur runs before the game's bloom composite. In some interiors a depth-off back-buffer draw happens mid-scene, so
  geometry drawn after it is not blurred (inferred, not observed).
- Thin alpha-tested foliage far away can show small sharp points inside the blur (a hypothesis about the composite
  fallback, not confirmed).
- Map view detection needs `Camera_IsMapViewModeEnabled`; on other game versions *Sharp in map view* does nothing and
  the card says so.
- The auto depth-stencil must be D24S8 or D24X8 at screen size.

## Technical reference

### Post-scene trigger

`features/post_scene.cpp` / `.h` provide one trigger for the effects that work on the finished scene.

- **Chain:** `kAmbientOcclusion = 10`, `kEdgeSmoothing = 20`, `kDepthBlur = 30`, kept with `std::stable_sort`; `Add`
  ignores duplicates. The first `Add` registers hooks under `"PostScene"` (Present, SetRenderTarget, DIP, DP, all
  `Priority::First`) and the `endSceneBeforeOverlay` and `preReset` callbacks; the last `Remove` unregisters them.
  Nothing runs until the next Present after registration.
- **Draw boundary** (`OnGameDraw`): ignored while the frame is done, while `DepthShare::InternalPass()` is set, while
  shader precompilation is incomplete, or when RT0 is not the back buffer. A draw with `ZENABLE` on counts as a scene draw.
  The first draw with `ZENABLE` off after at least `kMinSceneDraws = 20` scene draws is the boundary: the bloom
  composite when bloom is on, otherwise the first UI draw. The effects run inside that draw's callback, before it
  executes, so they see the scene without bloom or UI.
- **Depth check** (`SceneDepthReady`): when a shared depth exists, the bound depth-stencil must be
  `DepthShare::Surface()`. Otherwise the boundary is rejected without consuming the effects, and only resumed
  depth-tested scene draws with the right depth unlock another boundary.
- **EndScene fallback** (`AtEndSceneBeforeOverlay`): if no qualifying boundary happened (for example with the game UI
  hidden), at least 20 scene draws occurred, RT0 is the back buffer and the depth check passes, the chain runs once at
  the game's EndScene, before Picture's scene copy and the Apex overlay. It never runs over a rejected UI boundary.
- Effects draw with `DrawPrimitiveUP`, which the registry does not hook, so they never re-trigger the chain. Their
  `SetRenderTarget` calls do pass through the registry, so the RT0 tracker follows them and each effect restores RT0.
- Night Lighting's draw replacements return `Skip` at `Priority::Normal`; the trigger sees every draw first. Its pond
  pass turns `ZENABLE` off and unbinds the depth-stencil, so it is wrapped in `DepthShare::SetInternalPass(true/false)`.
- Camera votes (`CameraNear`, `CameraDepthA`, `CameraViewProj`) are available to effects that request them (Ambient
  Occlusion; Edge Smoothing reads the near plane). Depth Blur does not need them: the Auto focus uses depth ratios.

### Depth share

`features/depth_share.h`, implemented at the end of `depth_blur_patch.cpp`:

| Function | Returns / does |
|---|---|
| `Texture()` | The INTZ texture when ready, else null |
| `Surface()` | Its level 0, bound as the depth-stencil while the scene renders, when ready |
| `SetInternalPass(bool)` / `InternalPass()` | Marks another feature's own extra draws (render thread, plain bool) |
| `Request(bool)` | Reference-counted request to keep the swap running with Depth Blur off |
| `Status()` | Depth Blur's status string |

The swap runs while Depth Blur is on or `requests > 0` (`UpdateDepth`). Consumers must check that `Surface()` is the
depth-stencil bound now (`ExtraHooks::RawGetDepthStencilSurface`) before trusting the texture, and unbind it
(`RawSetDepthStencilSurface(nullptr)`) while sampling it.

`StartDepth()` registers a Present hook (`"DepthBlur"`, `Priority::First`) and the reset callbacks. `StopDepth()`
unregisters them, invalidates the world readiness and releases resources. `Install()` sets `blurOn` and `focusSnap`,
calls `UpdateDepth()` and adds the effect; `Uninstall()` removes it, calls `UpdateDepth()` (the swap stays for other
requesters) and releases the shaders. `Update()` never reinstalls: that would tear down the swap from the wrong place.

### ExtraHooks

`framework/d3d9_extra_hooks.cpp` detours device vtable slots the registry does not cover: 34 `StretchRect`,
39 `SetDepthStencilSurface`, 40 `GetDepthStencilSurface`, 43 `Clear`, 83 `DrawPrimitiveUP`, 84 `DrawIndexedPrimitiveUP`.
They are installed once on first `EnsureInstalled(dev)` (Depth Blur's `InitResources` or Frame Capture) and never
removed; with no callback set each costs an atomic load. Installation is refused when a slot shares code with a slot the
registry already detoured (DXVK can fold functions): `[ExtraHooks] vtable[m] shares code with vtable[o], not installing`.
The depth substitution has a single owner (`SetDepthSubstitution(substitute, report)`); observer slots serve Frame
Capture; `RawSet/RawGetDepthStencilSurface` bypass the substitution.

**Render callbacks** (`framework/render_callbacks.h`): lists `endSceneBeforeOverlay`, `filteredSceneBeforeOverlay`,
`preReset` and `postReset` grow as needed; `Fire` calls in registration order.

### Resources (`InitResources`, at Present)

- `ExtraHooks::EnsureInstalled(dev)`; failure: "ERROR: could not install the depth hooks".
- Back buffer not multisampled, else the MSAA status above.
- Bound depth-stencil of back-buffer size, not multisampled, `D3DFMT_D24S8` or `D24X8`; else "Waiting for the game (no
  depth buffer yet)" or "Waiting for the game (depth buffer is not the screen's)". Kept (AddRef) as `origDS`.
- **INTZ** texture `W x H` (`D3DUSAGE_DEPTHSTENCIL`, `MAKEFOURCC('I','N','T','Z')`) and its level 0; failure: "ERROR: the
  graphics card/driver does not support INTZ depth textures".
- **fullTex:** `W x H` render target in the back buffer's format (Prep colour source; 33 MB at 4K).
- **halfA / halfB:** `ceil(W/2) x ceil(H/2)`, `A16B16G16R16F` when `CheckDeviceFormat` reports it as a filterable render
  target and creation succeeds; else `A8R8G8B8` with `[DepthBlur] 16-bit float render targets not available, using
  A8R8G8B8 (blur in gamma space)` (linear light off: an 8-bit linear image would band in the darks). Alpha carries the
  blur amount between passes.
- **focus[2]:** two 1x1 targets, the first of R32F, R16F, `A16B16G16R16F` that is supported; none:
  `[DepthBlur] No 1x1 float render target (R32F / R16F / A16B16G16R16F): Auto focus falls back to Fixed`.
- Shaders `FocusPS`, `PrepPS`, `CompositePS` and `BlurPS` per quality (`TAPS` macro), ps_3_0,
  `D3DCOMPILE_OPTIMIZATION_LEVEL3`, "depth_blur.hlsl", compiled at start-up on a background thread; the render thread
  creates objects only. Failure: "ERROR: the blur shaders did not compile (see ApexRadiance_LOG.txt)", not retried every
  frame.
- 4 sets of timestamp queries.
- Log: `[DepthBlur] Resources ready (WxH, INTZ depth swapped in, blur targets A16B16G16R16F, focus target R32F)`.
- Not ready: retried every `kRetryFrames = 120` frames. Reset: `OnPreReset` clears the substitution, re-binds
  `origDS`, releases every default-pool resource, query and the read-out surface, and sets `focusSnap`; `OnPostReset`
  retries at the next Present.

### Loaded-world guard

Loading screens and menus can leave a stale depth buffer. `BlurEffect` runs only in an active loaded world:

- **World active** (`WorldSession::IsActive`, read-only, inside `__try`): the WorldManager global
  (`GameAddr::WorldManagerPtr`, `0x011ECBC4` on Steam 1.67.2, also found by signature) points to a manager whose byte
  at `+0x41` is non-zero and whose mode at `+0x1B4` is 1 to 3.
- **Loading window absent** (`WorldSession::LoaderDismissed`): the UI service getter (`GameAddr::UiServiceGetter`,
  `0x0050AB70` on Steam, found from the `UIManager_GetMainWindowImpl` call signature) must have the shape
  `mov eax, [global]; ret` (`A1 xx xx xx xx C3`). Apex reads the service, calls its vtable `+0x04` for the UI root, and
  calls the root's vtable `+0xF4` to look up child window id `0x95947678` (non-recursive). That window is created at
  `0x00EC7DB9` and removed by the callback at `0x00EC7A60`. Any missing state, unrecognised getter or fault counts as
  "loading" (fails closed). Apex never removes windows or changes loading.
- **Settled** (`WorldSession::Settled`): Present updates it every frame; it becomes ready after 3000 ms of continuous
  activity. A load, a missing or unreadable manager, a video reset (`OnPreReset`) or stopping the depth swap
  (`StopDepth`) resets it.
- **While not ready:** `BlurEffect` returns before any blur or debug pass and sets `focusSnap`, clears the frame timer,
  closes the map fade (`mapOpen = false`, `mapFade = 0`). The same reset happens at Present. Auto focus therefore snaps
  on return, and a long stall cannot feed a huge time step into the easing. `BlurEffect` re-checks `IsActive()` itself,
  so the blur stops at once when a load starts. The depth swap and saved settings are not affected.

The same gate defers the menu's startup ready notice. Field documentation:
[engine/lot-loading-and-streaming.md](../engine/lot-loading-and-streaming.md).

### Per frame (`BlurEffect`, post-scene order 30)

1. Return when Depth Blur is off, not ready, re-entered, inside an internal pass, or the world guard is not ready.
2. `StepTime()`: `dt` from QPC since the previous call, clamped to 0.1 s. `StepMapFade(dt)`:
   `mapOpen = offInMapView && MapView::IsOpen()`; the frame it closes sets `focusSnap`; `mapFade` moves toward 1 / 0 by
   `dt / 0.3 s`.
3. No GPU work when not in debug view and `strength x (1 - mapFade) <= 0` or `amount <= 0`.
4. Timestamp queries around `RunBlur`, read a few frames later without waiting (EMA 0.9 / 0.1, restarted when Quality
   changes); `GpuCostMs()` feeds the cost chip.
5. `RunBlur(dev, dt)`:
   1. `StretchRect(backbuffer -> fullTex, D3DTEXF_NONE)`.
   2. `SavedState::Capture`: RT0, depth-stencil (raw), PS, VS, declaration, FVF, stream 0, textures and 8 sampler states
      of s0..s3, 15 render states, PS constants c0..c6, viewport. No state block.
   3. Unbind the depth-stencil (the INTZ is sampled); Z, blend, stencil, fog and sRGB off, cull none; samplers point and
      clamp, no mips.
   4. Constants (below); INTZ on s2.
   5. **Focus** (Auto only): RT = `focus[1 - cur]`, s3 = `focus[cur]`, 1x1 quad; `cur` flips; `focusSnap` clears.
      Later passes read the new focus at s3 (null in Fixed).
   6. **Prep:** `fullTex` (s0, point) to halfA.
   7. **Gather H** halfA to halfB, **gather V** halfB to halfA (s0 linear).
   8. **Composite** into the back buffer: halfA on s1 (point), `SRCALPHA / INVSRCALPHA`, RGB write only (the game's alpha
      is kept).
   9. `SavedState::Restore`: RT0 first (it resets the viewport), stream 0 explicitly (`DrawPrimitiveUP` clears it), the
      depth-stencil with `RawSetDepthStencilSurface`.

### Constants

| Reg | x | y | z | w |
|---|---|---|---|---|
| c0 `cParams` | start (Fixed) | range (Fixed) | strength x (1 - mapFade) | farPlane (Fixed) |
| c1 `cHalf` | half W | half H | 1 / half W | 1 / half H |
| c2 `cDir` | gather dir x | gather dir y | max radius R (half-res px) = amount x 0.01 x H / 2 | lamp weight (2 or 0) |
| c3 `cFlags` | blur sky | debug view | linear light | auto focus |
| c4 `cFull` | W | H | 1 / W | 1 / H |
| c5 `cFocus` | c0 = 1 - 1/S | 1 / (c1 - c0), c1 = 1 - 1/T | A = 1.00008 | |
| c6 `cEase` | ease = 1 - exp(-dt / tau) (1 on snap) | 1 = ignore the previous focus | window half width (uv) = 0.025 H / W | window half height (uv) = 0.025 |

### Auto focus (`FocusPS`, GPU only)

- 16 depths on a 4 x 4 grid over a centred square of 5% of the screen height (`kFocusWindow`), each snapped to a
  full-resolution texel centre and point-sampled. Sky (`d >= 0.99999`) is replaced by 2 and never counts.
- Statistic: the **25th percentile** of the n non-sky samples, the t-th smallest with `t = max(1, ceil(n / 4))`,
  computed without sorting (a sample is a candidate when at least t samples are <= it: 4 `step` + 1 `dot` per sample;
  the smallest candidate wins). The nearer quarter wins, so a Sim covering a quarter of the window holds the focus
  against the background; a thin post or leaf covering one or two samples cannot grab it; one outlier cannot drag it.
- Stored value `F = A - d_f` (proportional to 1/z_f; the near plane cancels).
  `next = prev > 0 ? lerp(prev, F_target, ease) : F_target`. Easing in 1/z space matches a lens (blur is linear in 1/z).
- All samples sky: keep `prev`. Snap (ignore `prev`) after `InitResources` (so after every reset), on install, when the
  map view closes, when the mode switches to Auto and when the world guard becomes ready. A snap that sees only sky
  stores -1 ("unknown"): only the sky blurs until something is seen.
- Two 1x1 targets ping-pong; Prep and Composite read the new value with one point fetch per pixel. The CPU never reads it
  back.

### Blur amount per pixel (`BlurAmount`)

The game's projection is `d = A - near * A / z`, `A = 1.00008`, near 0.2 to 0.3
([engine/camera-and-map-view.md](../engine/camera-and-map-view.md)).

Auto (thin-lens ratio):

```
r = (A - d) / (A - d_f) = z_f / z          // near cancels
c = 1 - r                                  // 0 at the focus, -> 1 at infinity, < 0 nearer than the focus
k = saturate((c - c0) / (c1 - c0)),  c0 = 1 - 1/S,  c1 = 1 - 1/T
nearer than the focus (c < 0): k = 0;  focus unknown (stored value <= 0): k = 0
```

| Sharp area | S, T | c0, c1 | Blur starts / is full at |
|---|---|---|---|
| Small | 1.5, 4 | 0.333, 0.75 | 1.5x / 4x the focus distance |
| Medium | 2, 6 | 0.5, 0.833 | 2x / 6x |
| Large (default) | 3, 10 | 0.667, 0.9 | 3x / 10x |

Fixed (heuristic):

```
lin = d / (F - d * (F - 1))                 // F = farPlane (1000)
k   = saturate((lin - start) / max(range, 1e-4))
```

Both: sky gets `k = blurSky`; then `k *= strength x (1 - mapFade)`. The per-pixel radius is `r_px = k x R` (half-res
pixels) with `R = amount x 1% of the screen height / 2`, so the look is the same at any resolution.

Fixed distance in metres (near 0.25, F = 1000; the same setting moves about 20% with near 0.2 or 0.3, that is, with
zoom): `z = near * (1 + lin * (F - 1)) / (1 - lin)`. Near 0.25 is about 84 m, Medium 0.349 about 134 m, Far 0.45 about
205 m. The default start 0.318 is about 117 m, with full blur at 0.606 (about 385 m).

### Prep (`PrepPS`, one half-res texel per 2x2 block)

- Block top-left = `floor(uv * halfSize) * 2 + 0.5` (full-resolution texel centres).
- 4 point depths give 4 k values; 4 colours are linearised with `pow(c, 2.2)` (FP16 targets only).
- Block amount `kmin = min(k0..k3)`: a block touching a sharp edge counts as sharp, so the gather never spreads it into
  the background.
- Block colour weights `w_i = saturate(1 - (k_i - kmin) x max(R, 2))`: pixels whose radius is within about one half-res
  pixel of the block's. The sharpest pixel always has weight 1; a uniform block is an exact 2x2 box.

### Gather (`BlurPS`, separable, half resolution)

```
r = centre.a * R                      // half-res pixels
r < 0.05: output = centre             // sharp pixels cost one fetch
taps i = -TAPS..TAPS at offset (i/TAPS) * r along the axis (bilinear)
w_i  = exp(-2 (i/TAPS)^2)             // Gaussian, sigma = r/2
w_i *= saturate(tap.a * R - |i/TAPS * r| + 1)                    // scatter-as-gather acceptance
w_i *= 1 + lampWeight * centre.a * max(luma_lin(tap) - 0.8, 0)   // Glowing lights, at most 1.4x
output = (sum w_i tap.rgb / sum w_i, centre.a)
```

H then V; alpha carries the radius. The radius grows with k, so there is no sharp-plus-ghost double image. No per-frame
noise, rotation or temporal accumulation: identical input gives identical output.

### Composite (`CompositePS`, full resolution, alpha blended)

```
k (full-res pixel, point depth), r = k * R
alpha = smoothstep(0.1, 1.0, r)         // fully blurred from 2 full-res px of radius
alpha == 0: output alpha 0 -> the original pixel exactly
4 nearest half-res texels (point), bilinear weights bw_i
dr_i = |k_i - k| * max(R, 2)
w_i  = bw_i * exp2(-2 dr_i^2)
colour = sum w_i q_i / sum w_i -> pow(1/2.2) (FP16 only)
alpha *= saturate(sum w_i * 20)         // no similar texel around: fall back to the original
debug: (k, k, k, 1), focus window tinted violet in Auto
```

### Shaders (`kShaderSource`, ps_3_0)

| Entry | Inputs | Output | Estimated slots |
|---|---|---|---|
| `FocusPS` | s2 INTZ, s3 previous focus, c4 to c6 | focus[next], 1x1 float | about 350 (16 fetches) |
| `PrepPS` | s0 fullTex (point), s2 INTZ, s3 focus, c0 to c5 | halfA: rgb linear colour, a = kmin | about 120 |
| `BlurPS` (`TAPS` 4/6/8/12) | s0 half target (linear), c1, c2 | the other half target | about 300 at Ultra |
| `CompositePS` | s1 halfA (point), s2 INTZ, s3 focus, c0 to c6 | back buffer RGB, alpha-blended | about 70 |

Slot counts are estimates; all are under the ps_3_0 minimum of 512. Every fetch is `tex2Dlod` (inside dynamic branches).

### Map view detection (`features/map_view.cpp`)

There is no D3D-level signal for the map view, so `MapView::IsOpen()` calls the game's script binding:

1. `Resolve()` (once, on the first `Available()` / `IsOpen()` call from the render thread) finds `.text`, `.rdata` and
   `.data` of `TS3W.exe`.
2. It searches `.rdata` for `"ScriptCore.CameraController::Camera_IsMapViewModeEnabled"` with its terminator (Steam
   1.67.2: `0x010000A4`).
3. The script API registers native calls as `{function pointer, name pointer}` pairs. It searches `.data` (4-byte
   aligned; fallback `.rdata`) for the name's address; the dword before it is the function pointer. Steam 1.67.2:
   `.data 0x0115DD40` (function) / `0x0115DD44` (name). The code comment's `0x0115DD20` is the neighbouring entry
   `{0x0073D620, "ScriptCore.CameraController::Camera_SetMotion"}`; the search does not use the constant.
4. `LooksLikeGetter` validates the pointer: inside `.text` with 0x60 bytes to spare, starts with `E8`, pushes the cast id
   `68 89 DD 0F 11` (`push 110FDD89h`) and ends in `8A 80 xx xx xx xx C3 32 C0 C3` within 0x50 bytes. Otherwise:
   `[MapView] Unexpected function at 0x..., map view detection off`.
5. Found: `[MapView] Camera_IsMapViewModeEnabled at 0x0073e060`.
6. `IsOpen()` calls it as `bool __cdecl()` inside `__try/__except`; a fault logs `[MapView] Call faulted, map view
   detection off` and disables detection for the session.

`0x0073E060` calls `0x0096B390` (app), `0x00F20C20` (world), `0x0096B6D0` (camera manager), then the manager's vfunc
`+0x0C` with cast id `0x110FDD89`, and returns the byte at camera `+0x8B9`, or 0 when any link is null. It only reads,
so the render thread may call it. The fade is a linear ramp of `kMapFadeSeconds = 0.3` on `mapFade`.

### Developer read-out

While the Developer page draws, it asks for a read-out for the next 0.5 s. `RunBlur` then copies the focus value to a
1x1 system-memory surface (`GetRenderTargetData`, at most 4 times a second) and issues an event query; a later frame
reads it only when the query is done. The metre value assumes near 0.25 (`z = 0.25 x A / (A - d_f)`), so it is
approximate. The page shows the focus mode (`Auto`, `Fixed`, or `Auto (no float target: using Fixed)`), the focus
depth, GPU cost, blurred frames, taps per side, blur target format and map-view state, under *Focus and rendering
details*.

### Addresses (Steam 1.67.2)

| Address | What |
|---|---|
| `0x0073E060` | `Camera_IsMapViewModeEnabled` (returns camera `+0x8B9`), found at run time |
| `0x011ECBC4` | WorldManager global (`+0x41` active, `+0x1B4` mode) |
| `0x0050AB70` | UI service getter (`mov eax, [global]; ret`) |
| `0x00EC7DB9` / `0x00EC7A60` | Loading window (id `0x95947678`) creation / removal |
| vtable 34/39/40/43/83/84 | D3D9 device methods detoured by ExtraHooks |

### Cost (estimated, not measured)

At 3840 x 2160 on an RTX 4070 Ti SUPER, Quality High: copy about 0.1 ms, Prep about 0.15 ms (8 fetches per half-res
texel, 66 MB read), gather H + V about 0.2 to 0.3 ms (2 x 17 bilinear FP16 taps on 2 Mpx, sharp pixels exit early),
composite about 0.2 ms, focus negligible: about 0.6 to 0.8 ms (Ultra about 0.9 ms). Memory: INTZ 33 MB, full copy
33 MB, two 16.6 MB FP16 targets. The card shows the measured value.

## Rejected approaches

- Depth from one point sample per half-res pixel: picks a different pixel after a 1-pixel camera move.
- Per-frame noise, rotation or temporal accumulation: twinkling on leaves and at disocclusions.
- Spreading a fixed kernel's taps wider: dotted artefacts; larger radii need more taps.
- Cross-fading a fixed blur by a mask: sharp image plus ghost in the transition.
- CPU readback of the focus: stalls the pipeline.
- Full state blocks: CPU heavy.
- Reinstalling from `Update()`: tears down the depth swap from the wrong place.

Details in [history](../history/depth-blur.md).

## See also

- [Validation](../validation/depth-blur.md)
- [History](../history/depth-blur.md)
- [Engine: camera and map view](../engine/camera-and-map-view.md)
- [Engine: lot loading and streaming](../engine/lot-loading-and-streaming.md)
- [Architecture: post-scene chain and INTZ depth share](../architecture.md)

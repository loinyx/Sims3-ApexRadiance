# World light atlas and smoothed terrain light maps: validation

The feature is described in
[features/night-lighting/world-atlas-and-smoothed-maps.md](../features/night-lighting/world-atlas-and-smoothed-maps.md).
It must:

- Never show a smoothed map built from an older game map, and never show black ground light while a chunk is rebuilt,
  the atlas grows or the device resets.
- Give the same look on the GPU and CPU paths: at most 1 per channel at level 0, most texels identical, levels 1 to 10
  at most 1.
- Keep the light level of the game map (only the colour ratio is blurred) and keep the light-map alpha (sun
  visibility) identical to the native map.
- Apply *Ground brightness* equally to linear and squared terrain lamp constants, at every day/night weight.
- By day, restore the lamp term on the captured single-pass shader only up to the multi-pass composition ceiling, with
  twilight blending smoothly and the night output unchanged.
- Leave unsupported shader layouts, unreadable maps and failed shader creation on the game's own map.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test`](../../tools/terrain_lighting_test/README.md) (`check.cpp`) | `TerrainLightingPolicy` (day zero factors, single-pass and squared multi-pass factors, 101 night levels, 120 gains, 21 native scales, previous night rounding, invalid values); native-alpha patch on SM2 and SM3 bytecode, malformed, truncated, ambiguous and unsupported layouts; GPU readback: smoothed RGB kept, solar alpha identical to the native map; exact `NativeTerrainSampler` restoration after every injected read/set failure and no redundant writes; captured shader patching and creation; exclusive c7 constant identification | `run.ps1 -Captures ... -OutDir ...` | Native D3D9 GPU; fixtures `PS_2BA33118.bin` (world s7) and `PS_2BA62940.bin` (multi-pass s2) from the 2026-10-04 02-05-41 session |
| `check.cpp` with `-CompositionCapture` | Daylight range: the 11-52-03 single and multi-pass shaders with equal material, normal, sun and lamp inputs; all RGB pixels at six lamp levels, lamp-free pixels preserved, three twilight weights, zero-weight night identity, rejection of foreign or mutated bytecode | `run.ps1 ... -CompositionCapture '<11-52-03 F7 folder>'` | Native D3D9 GPU |
| `ground_scale_check.cpp` | Linear vs squared constant classification on the captured s7/s11 shaders; refusal of shared, mixed or saturated multipliers; GPU readback of both multiplier equations at five day/night weights and eight gains | `run_ground_scale_checks.ps1 -Captures '<2026-10-04 15-33-55 session>'` | Native D3D9 GPU |
| `fxc` | Each smoothing entry point compiles alone: `fxc /T ps_3_0 /E GatherPS /O3 lightmap_smooth_ps.hlsl`, and the same for `HBlurPS`, `VBlurPS`, `HUpYAPS`, `HUpChromaPS`, `VUpPS`, `DownPS`, `CopyPS` | manual | Windows SDK |

The composition fixture is not an exact replay of scene vertices, textures, depth or all material layers. The
sampler-guard cost it reports is an isolated driver CPU benchmark, not game FPS.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | `92582b6` | GPU and policy suite including the daylight range candidate | 2,003,751 passed, 0 failed | Native D3D9 |
| 2026-10-04 | `92582b6` | Extracted resource suite | 13,642 passed, 0 failed | Native D3D9 |
| 2026-10-04 | `92582b6` | Composition fixture, 0.2 material and 1.35 sunlight, full lamp-map RGB at the captured gain | Native: single pass 131, multi-pass 102; both 69 without lamps; candidate matches the multi-pass ceiling | Native D3D9 |
| 2026-10-04 | `622a93c` | Ground scale checks | Linear and squared equations agree at every tested weight and gain | Native D3D9 |

## In-game test plan

GPU path (default):

1. Load a world. **Expected:** log `[LightmapSmooth] GPU smoothing ready (intermediates A32B32G32R32F, ...)` and
   `Smoothing on the GPU`; otherwise `GPU smoothing unavailable (why): using the CPU path`.
2. Developer > Lighting, "Smoothed light map" status. **Expected:** `GPU (F intermediates) | chunks smoothed: R of N |
   waiting: W (neighbour borders B) | built: X (in view, out of view, borders), failed Y | atlas cells written | GPU
   time: T ms per chunk, last batch, max batch (n timed) | changes seen (render notices, same map), hash checks | world
   map ... | GPU vs CPU: ...`. "failed" stays 0; W returns to 0 right after each change in view.
3. Press "Compare GPU vs CPU (one chunk)" looking at lit ground. **Expected:** level 0 max diff 0 or 1 per channel,
   "by more: 0"; levels 1-10 max diff at most 1. A larger difference is a math mismatch.
4. Dusk, or a lamp edit at night, looking at a lamp. **Expected:** the ground switches to the new light smooth at once
   (no blocky frame); lots, floors and fences next to it in the same frame. With the developer A/B off: blocky for a
   moment, then smooth.
5. Frame Profiler / hitches during a dusk rebuild. **Expected:** the GPU time per chunk in the status; no hitch from
   smoothing.
6. Alt+Tab or a resolution change (device reset). **Expected:** the ground light comes back smooth at once, no black
   cells on lots.
7. Frame Capture or census around a rebuild. **Expected:** the other modules' trackers are unaffected by the passes drawn
   inside their draw hooks.

CPU path (developer A/B off, or fallback):

8. Status. **Expected:** `CPU | chunks smoothed: R of N | waiting: W (in flight F, queue Q) | jobs: J, avg A ms, max M ms
   | skipped (inputs unchanged): S | uploaded: U (later L, outdated O) | plain ground copies: P | changes seen: C (render
   notices K) | unreadable: X | world map: WxH chunks (copies, grown G x) | [holding: rebuild coming] | [rebuild sweep: N
   chunks not re-rendered yet] | last kick: reason (s ago), last change T ms after it | last sweep: kick -> rebuild, ->
   first chunk, -> last chunk`. R counts only smoothed maps of the current game map; W returns to 0 after a rebuild.
9. Developer log. **Expected:** `Rebuild sweep done: ...` after each rebuild; with verbose logging, one `Chunk (x, z)
   changed (render notice | new texture | fast check | round robin), T ms after the last kick` per chunk in view.
10. Dusk or a Build-mode lamp edit at night while looking at a lamp. **Expected:** the ground under it switches at once
    (blocky for a moment, then smooth); lots, floors and fences follow in the same frame (atlas plain copy); no frame
    shows the old light or black after the game's map changed.
11. Pan quickly across the world. **Expected:** no black ground light at newly loaded chunks or elsewhere when the world
    map grows ("grown G x" increments, log `World map grown ... (old contents copied)`).

Both paths:

12. Toggle *Smooth ground light* at night on snow. **Expected:** specks and 1 m blocks disappear when on.
13. F7 on world grass. **Expected:** the terrain draw's light-map sampler shows a 1024x1024 A8R8G8B8 texture with 11
    mips; the native map on the spare sampler; atlas consumers show the 2D render target (sizes seen: 2560x2560,
    4096x4096). The `mod draw: world terrain` line names the variant, gain and `daylight range candidate` when it applies.
14. Log. **Expected:** the `unreadable` line names chunks the game rebuilt in a non-lockable format.
15. *Ground brightness* away from 100% at a border between a single-pass and a squared-constant chunk, on grass and
    snow, by day and night. **Expected:** no brightness step.
16. Daytime Build mode with lamps lit, at a lot border on the captured single-pass terrain. **Expected:** world and lot
    grass show the same subdued lamp term; sunlight and shadows unchanged from the game.

## Confirmed in game

- Smoothing (decode, colour cleanup, 4x B-spline, mips) since the v0.1.0 baseline (`b84d5f1`): blocks and specks
  removed.
- 2026-10-02, terrain-variant build `C1257AF4...`: with the summer multi-pass pair recognised, the reported lot/world
  cutoff resolved in the tested scene (maintainer feedback, no new GPU comparison).

## Open checks

- GPU path log lines, compare result and per-chunk GPU time have not been recorded in a test report.
- GPU path assumptions: the instruction limits of the largest entry point `VUpPS` (about 6 texld + 60 ALU); DXVK 3.1.1
  support for `A32B32G32R32F` (else `A16B16G16R16F`) render-target textures with point sampling and for mipmapped
  A8R8G8B8 render-target textures with each level as a target; point sampling of the game's DXT5 maps (managed or
  render target); `D3DFVF_XYZRHW` with ps_3_0 and absolute coordinates with the viewport at the whole atlas; drawing and
  `GetRenderTargetData` inside the game's draw hooks; several timestamp queries per frame; NPOT render targets (264,
  1024x264) with CLAMP and one level.
- Whether `D3DLOCK_DONOTWAIT` on a SYSTEMMEM texture returns `D3DERR_WASSTILLDRAWING` under DXVK 3.1.1 when busy (else it
  waits: correct but may stall), and whether `StretchRect` between the two atlas render targets succeeds under DXVK
  (fallback: plain copies, logged). Check "later" and "grown ... old contents copied".
- Whether the game's rebuilt maps are managed DXT5 or render targets: the GPU path samples either; only the fallback
  hash check and the compare need a lockable map.
- PR #2: native solar alpha, the daylight range candidate and squared ground scales in gameplay (same-lot scene, snow,
  seasons) and under DXVK. No FPS change is claimed.
- Winter multi-pass variants and other untested scenes for the multi-pass and compact corrections.
- Planned, not started: PASSO3 increment 5, re-rendering the terrain stamp at 4 texels/m with the game's formula from
  `FUN_00C292B0` (`E = c0.rgb * sat(c0.w / d^2) * (N.L >= 0 ? sat(sqrt(N.L)) : 0)`, PASSO3-PLANO section 4), delivered
  through `LightmapSmooth::Find` and validated against the original at 256^2. Pre-sizing the atlas to the whole terrain
  at world load (terrain chunk vector `terrain+0xB0/+0xB4`) needs the WorldManager global and the chunk coordinates at
  run time; the GPU copy on growth already avoids black cells.

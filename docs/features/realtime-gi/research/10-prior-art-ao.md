# Prior art for Apex Radiance's four dropped AO attempts

Online prior art that addresses the four AO failures recorded in [00-camera-pan-profile.md](00-camera-pan-profile.md)
§9 and [ambient-occlusion.md](../../../ambient-occlusion.md), checked against Apex's shader
(`patches/ambient_occlusion_patch.cpp`, `GtaoPS` / `BlurPS` / `CompositePS`, ps_3_0) and its constraints. Companion to
[09-salvage-apex-dropped.md](09-salvage-apex-dropped.md), which salvages the same ideas by reasoning; this file adds the
published techniques, their licences and concrete pass changes. Desk research only: no game run, no measurement. Costs
are **inferred** from the published numbers and Apex's own pass costs (4K, RTX 4070 Ti SUPER, native D3D9: High 3.0-3.2 ms,
0.55 ms fixed). Through the RTX Remix bridge every pass is one more D3D call sequence crossing the process boundary, but
none of the proposals below adds a read-back.

## 0. Constraints that filter the prior art

| Constraint | Effect on what can be ported |
|---|---|
| ps_3_0 only (no compute, no `Gather`, no integer or bit operations, no groupshared memory, 4 MRT, 16 samplers, 224 float constants) | XeGTAO / CACAO compute passes must become full-screen pixel passes; bit-packed edges become float channels; a 32-bit visibility bitmask has no native type |
| Every draw goes through the bridge to a 64-bit process | Pass count matters a little (each pass is a few calls); read-backs (~2 ms sync) are out |
| No game motion vectors, no normal target (forward renderer); the camera view-projection is known (`PostScene::CameraViewProj`, c40..c43; [camera-and-map-view.md](../../../engine/camera-and-map-view.md)) | Temporal reuse can reproject the camera only; Sims, foliage in the wind and moving objects need rejection, not motion |
| Apex is MIT (`LICENSE`, copyright 2026 loinyx) | MIT code (XeGTAO, ASSAO, CACAO) can be ported with the notice. NVIDIA HBAO+ (NVIDIA Source Code License, 1-way commercial), NVIDIA STBN (non-commercial licence) and MXAO/qUINT (all rights reserved; older MXAO reported CC BY-NC-ND) are **ideas only**: no code or shipped textures |

## 1. Failure 2: dashed lines along mesh seams (smoother normals)

**Cause (from 09 §4, inferred):** the "even surface" test was a binary switch; along a counter top's triangle edge the
depth has a tiny crease, so the switch flips pixel by pixel and the normal alternates between two forms.

### Prior art

| Technique | What it does | Fits ps_3_0 | Cost at 4K (inferred) | Licence |
|---|---|---|---|---|
| **Turánszki, "Improved normal reconstruction from depth"** (Wicked Engine, 2019) | 3 taps per axis, picks the neighbour with the smaller depth step. This is what Apex already ships | yes | baseline | blog code, no licence stated: reimplement |
| **Yuwen Wu, "Accurate Normal Reconstruction from Depth Buffer"** (2021) | 5 taps per axis (±1, ±2). Extrapolates each side's outer pair to the centre **in 1/z**: `he = abs(H.xy * H.zw / (2 * H.zw - H.xy) - d)`, and takes the side with the smaller error. Fixes the cases Turánszki's 3 taps miss (centre on the far surface's segment) | yes: 4 more `tex2Dlod`, a few ALU | +0.03-0.06 ms (4 point reads of an R32F already in cache) | blog code, no licence stated: reimplement |
| **XeGTAO `XeGTAO_CalculateEdges` + `XeGTAO_CalculateNormal`** (Intel, MIT) | Edges are **slope-adjusted**: `edges = min(abs(d), abs(d + slope))` with `slope = (right - left) / 2`, then `saturate(1.25 - edges / (0.011 * z))`, a soft 0..1 weight, not a switch. The normal is the **weighted sum of four quadrant cross products** (L×T, T×R, R×B, B×L), each weighted by the product of its two edge weights (+0.01) | yes, all ALU on the 4 taps Apex already reads | ~0 to +0.02 ms | MIT, can be ported with the notice |

### Recommendation

Use the XeGTAO form: it reuses Apex's four existing neighbour reads, has no binary decision anywhere (so a crease
cannot flip it per pixel), and on a plane all four quadrant normals are equal, so a flat counter top gets the
triangle's plane exactly. Its relative threshold (1.1% of z) treats a sub-millimetre seam crease as "same surface",
while the slope adjustment keeps slanted floors from reading as edges. Wu's 5-tap extrapolation is the fallback if
the lab still shows dashes at silhouettes (it is the more accurate of the two at real depth discontinuities).
09's mitigation 4a (continuous left/right weight) is the same idea in 1-D; the quadrant version also blends the axes.

**Integration (`GtaoPS`, ~20 lines):** replace the two `abs(zr - z) < abs(zl - z) ? ... : ...` lines with the
four-quadrant weighted normal; also output the four edge weights (they feed §2's denoiser). Gate: lab seam metric
from 09 §4 (pixels > 5 degrees off the true plane along the counter seam) and the lab shift metric must not rise.
**Quality gain:** visible on every flat surface with tessellation seams (counters, floors, terrain tiles); realism:
no stray contact shade where nothing meets.

## 2. Denoiser: replace the depth-ratio box/tent with an edge-aware filter

Not one of the four failures, but it is what makes §3 and §4 viable and it removes the haloing of the current blur.

### Prior art

- **XeGTAO denoiser** (MIT): per pixel, packed L/R/T/B edge weights from the main pass; edges made symmetric
  (`edgesC *= (edgesL.R, edgesR.L, edgesT.B, edgesB.T)`); a 3x3 kernel with diagonal weight 0.425 built from edge
  products; a small "leak" when 3-4 edges are set to cut aliasing; 1 pass with TAA, more without.
- **ASSAO "smart blur"** (Intel, MIT): the same edge packing, applied per deinterleaved quarter (see §4).
- **GTAO paper** (Jimenez et al. 2016, §5): 4x4 spatial noise cancelled by a 4x4 bilateral filter, then temporal.
  Apex already follows this spatial half.

### Fit and integration

The 4x4 Bayer interleave must still be cancelled exactly, so keep the **4x4 box support** but take its weights from the
XeGTAO edges instead of the 1/z ratio: pass 1 horizontal 5-tap box (0.5 1 1 1 0.5) with each tap weighted by the
product of edge weights along the path; pass 2 vertical likewise; drop the 2-pass tent (−0.1 ms, about half the blur
cost) or keep one 3x3 XeGTAO-style pass if the lab shows residual pattern. Edges need a channel: switch the AO target
from G16R16F (R = visibility, G = 1/z) to A16B16G16R16F with B = horizontal edge pair, A = vertical edge pair packed as
`l + r * 4` with 2-bit levels (XeGTAO uses 2 bits per edge, 4 levels; floats hold that exactly). Cost: +0.05 ms of
bandwidth for the wider target, −0.1 ms tent: net ≈ −0.05 ms. Quality: sharper contact shade at chair legs and rails,
no AO bleeding across silhouettes (the 1/z ratio test passes a far wall's shade onto a near rail of similar 1/z
ratio at distance).

## 3. Failure 1: slices by reach on screen went blotchy

**Cause (09 §3, verified):** the box cancels the 4x4 interleave only when all pixels in a 4x4 block share the same
direction set; a per-pixel count broke that.

### Prior art

| Source | Mechanism | What carries over |
|---|---|---|
| **ASSAO** (Intel 2016, MIT; [article](https://www.intel.com/content/www/us/en/developer/articles/technical/adaptive-screen-space-ambient-occlusion.html)) | Highest preset runs a cheap base pass, builds a **low-resolution importance map** (where AO varies), **blurs** the map, then picks the sample count per pixel from it; the kernel is **progressive** (a low count is a prefix of the high count), so neighbouring counts estimate the same integral | Count from a smoothed low-res map, not per pixel; nested sample sets |
| **FidelityFX CACAO** (AMD, MIT) | ASSAO in compute, same adaptive idea | confirms the design; compute code not portable |
| **XeGTAO quality presets** | One count for the whole frame (Low 2x4, Medium, High 3x6, Ultra) | 09's mitigation 3a |
| **Keller & Heidrich, "Interleaved Sampling"** (EGWR 2001) | The pattern cancels only when every cell of the tile uses the same per-cell sample set | the formal reason for the failure |

### Recommendation and one correction to 09

1. Ship 09's **3a first** (one count per frame from zoom/camera height, hysteresis): every pixel shares the set, zero
   risk to the interleave. This is XeGTAO's preset model with automatic selection.
2. For per-region counts take ASSAO's structure: an importance map at **1/16 resolution** (one texel per 4x4 block,
   from pyramid level 2: the projected large radius `Rl * cView.z * w0` in pixels), then a **3x3 max dilation** so
   the count changes only between whole 4x4 blocks and never inside the box support of a high-count block's neighbours.
   `GtaoPS` reads the map with point filtering and loops `[loop] for (s < count)` (ps_3_0 dynamic loops are fine).
3. **Correction to 09 §3b:** the nested set "4 slices = every other angle of 8, `(2s + b1) pi / 8`" keeps the angles
   inside the first half of each π/4 stratum for all 16 Bayer values (b1 ∈ [0, 1)), so the 4-slice blocks get a
   directionally biased set (anisotropic shade on oriented edges). Use ASSAO's rule instead: each count gets its own
   full stratification, `(s + b1) pi / count`, and only the **block** boundary changes count. Both sets are unbiased
   estimators of the same integral, so a boundary is noise-level, not a bias step. Where a seam remains, use the
   higher count in the 1-block band at the border (cost: the band only).

**Cost at 4K (inferred):** importance pass 0.02 ms (960x540, one read); a typical mid/far frame with half the blocks at
4 instead of 8 slices saves ~0.6-0.9 ms of the ~2.6 ms slice cost. **Quality:** unchanged if the lab gate passes (09:
tile-edge ratio <= 1.1, shift metric not up). **Realism:** neutral; the win is ms.

## 4. Failure 3: half resolution gave "micro dots"

**Cause (09 §5, verified):** a 2x2 reduction (one pixel, later the average) is not shift-stable: a 1-px camera move
picks another representative, so edge pixels flip between surfaces.

### Prior art

| Technique | Source | Fit | Note |
|---|---|---|---|
| **Deinterleaved rendering** (16 quarter-res layers, each one Bayer cell; every pixel still gets its own AO, nothing is upsampled) | Bavoil, NVIDIA [Deinterleaved Texturing sample](https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/d3d_samples/deinterleavedtexturingsample.htm), used by HBAO+ and ASSAO | ps_3_0: deinterleave with 4 MRT x 4 passes, then 16 quarter-size AO draws (the Bayer `b1, b2` become per-draw constants), reinterleave in the first blur pass by address math | **No dots by construction** (full resolution output). The gain is cache locality; Apex already reads coarse mips for far steps (McGuire's [SAO](https://research.nvidia.com/sites/default/files/publications/McGuire12SAO.pdf) mip trick), which takes much of that gain, so expect 10-25% of the march, not HBAO+'s ~2x (inferred). Contact steps start at 2 px, finer than a layer's 4-px grid: read step 0-1 from the full-res level 0 |
| **Min/max checkerboard downsample + nearest-depth upsample** | Jansen & Bavoil 2011, [Opacity Mapping white paper](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/OpacityMappingSDKWhitePaper.pdf) (nearest-depth upsampling); Pettineo, [The Order: 1886 use](https://therealmjp.github.io/posts/programmable-sample-points); Black Ops 3 min/max checkerboard | ps_3_0: yes | The reduction picks min 1/z on even and max on odd pixels of a checkerboard, so both sides of an edge survive; the upsample takes the one low-res sample whose depth is nearest the full-res depth (no blend at edges, bilinear inside). Removes most dots; foliage edges remain (09 §5a agrees) |
| **Joint bilateral upsampling** | Kopf et al. 2007 | yes | Weights by depth and (from §1) normal; softer than nearest-depth but leaks at thin rails |

**Recommendation:** keep full resolution as default. If a GPU-bound preset is needed (laptop GPU at 4K, Steam Deck),
build **deinterleaved full-resolution AO** first: it is the only option with no upsampling and therefore no dot
mechanism. Half resolution with checkerboard min/max + nearest-depth upsampling only for Very Low, behind the lab's
shift gate. **Cost:** deinterleave 0.1 ms + 16 small draws (16 more bridge draw sequences) against ~0.3-0.6 ms
saved at High (inferred, measure first). Half res would save ~1.5 ms at High but costs image stability.

## 5. Failure 4: temporal AO breathed and trailed

**Cause (09 §5, verified):** v1 clamped history to an already-averaged range and followed the noise; later versions
failed where the depth test broke (leaves in the wind) and showed the raw frame.

### Prior art

| Technique | Source | What it fixes |
|---|---|---|
| Neighbourhood clamp to the **current raw** AO's 3x3 min/max (before the spatial filter) | Karis, "High Quality Temporal Supersampling" (SIGGRAPH 2014 Advances) | v1's mistake: clamping to an averaged range removes the bound on noise |
| **Variance clipping** (mean ± γσ of the 3x3 neighbourhood, clip towards the mean) | Salvi, [An Excursion in Temporal Supersampling](https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf) (GDC 2016) | Tighter than min/max: less ghosting, less breathing |
| Camera-only reprojection from depth: `prevClip = PrevViewProj * worldPos(depth)`; **disocclusion test** on the history's stored 1/z (Apex's AO target already stores 1/z in G) with a relative threshold | standard TAA practice; GTAO paper §5 uses reprojection plus depth rejection | Trails behind Sims and moving objects without motion vectors: the depth mismatch drops the history |
| **Fixed per-frame rotation schedule** (XeGTAO: Hilbert-curve index into the R2 sequence, 64x64 tile, `+288 * (frame % 64)`) | XeGTAO README: blue noise caused temporal artifacts and was replaced | Breathing from noise whose statistics change frame to frame |
| Spatio-temporal blue noise | NVIDIA STBN ([paper](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks), [repo](https://github.com/NVIDIA-RTX/STBN)) | Better spatial and temporal spectrum when frames are accumulated. **Licence:** the repo's code and textures are under a non-commercial NVIDIA licence: generate masks with a public void-and-cluster implementation instead. Without accumulation it is worse than the 4x4 Bayer + 4x4 box, which cancels exactly |

### Recommendation

The prior art fixes the mechanisms, but the gain is small on this game (09 §5b: the moving camera is where frames
matter, and there the history must be dropped anyway). The one case worth it: "fewer slices plus accumulation when the
camera is still", that is Low (4 slices) with an 8-frame rotation schedule converging to beyond-Ultra quality on a
still frame. Design: keep the 4x4 Bayer for the spatial cancel and rotate the slice phase per frame by the R2 step
(`frac(b1 + frame * 0.618)`); history weight 0 while `SampleCameraMoving()`, ramp to 0.9 over 300 ms still; variance
clip on the raw AO; depth rejection on G. Leaves in the wind fail the depth test by design, so they show the
deterministic spatial AO, not a raw noisy frame (the v3 failure), because the fallback is the filtered current frame.
**Cost:** +1 history read/write (A16B16G16R16F ~0.15 ms at 4K), −1.2 ms (High to Low). **Rank:** after §1-§3, opt-in.

## 6. Other prior art checked

| Technique | Verdict for Apex |
|---|---|
| **Visibility bitmask** (Therrien, Levesque, Gilet 2023, [arXiv 2301.11376](https://arxiv.org/abs/2301.11376)) | Replaces the two horizons with an N-sector occupancy mask and a constant thickness: light passes behind thin objects (less over-darkening behind chair legs and rails). Needs integer bit operations: ps_3_0 has none, so it means emulating e.g. 8 sectors in two `float4` masks per slice side. Apex already approximates thickness (depth counts 1.3x, isolated pixels fade) and dropped "visibility-bitmask detail" in 2.11.1. Low priority; revisit only if thin-object halos are reported |
| **HBAO+** ([repo](https://github.com/NVIDIAGameWorks/HBAOPlus)) | Ideas only (deinterleaving, §4); licence is NVIDIA's 1-way commercial source licence; Apex's history already rejected HBAO's look (plain multiply, grey) |
| **ReShade MXAO / qUINT** ([repo](https://github.com/martymcmodding/qUINT)) | All rights reserved: no code. Its public ideas (half-res with depth-aware upscale, smoothed normals) are covered above by MIT or paper sources |
| **ASSAO / CACAO** ([ASSAO](https://github.com/GameTechDev/ASSAO), [CACAO](https://github.com/GPUOpen-Effects/FidelityFX-CACAO)) | MIT; ASSAO's own README calls it superseded by XeGTAO. Take the importance map and progressive kernel (§3) and the edge-packed smart blur (§2) |
| Ambient-only darkening (the 2026-09-30 "ambient share" plan in the AO history) | The largest **realism** gain left: AO should darken only ambient light, not lamp light. Every engine above composites AO into the ambient term only. Apex approximates this with `protegerLuz`. Out of scope here; it needs shader patches (history doc order) |

## 7. Ranking by quality per ms

| Rank | Change | Fixes | GPU at 4K (inferred) | Quality / realism | Licence | Effort | Gate |
|---|---|---|---|---|---|---|---|
| 1 | XeGTAO slope-adjusted edges + quadrant-weighted normal (§1) | Failure 2 | ~0 | Seam dashes gone; correct plane normals | MIT port | ~1 h | Lab seam metric + shift |
| 2 | Edge-aware 4x4 box from XeGTAO edges, drop tent (§2) | haloing; enables 3 | −0.05 | Sharper contact shade, no bleed | MIT port | ~2 h | Lab shift, still-camera screenshot pair |
| 3 | One slice count per frame by zoom (09 3a, XeGTAO presets) | Failure 1 (safe half) | −0.5 to −1 far | Same image (within 0.4-0.6/255 measured) | own | ~1 h | Lab max diff |
| 4 | ASSAO-style per-4x4-block count, own stratification per count (§3) | Failure 1 (rest) | −0.6 to −0.9 mixed | Same if tile-edge ratio <= 1.1 | MIT ideas | ~2-3 h | Lab tile-edge + shift |
| 5 | Wu 5-tap 1/z extrapolation at silhouettes (§1 fallback) | residual 2 | +0.05 | Cleaner silhouettes | reimplement | ~1 h | Only if 1 leaves dashes |
| 6 | Deinterleaved full-res march (§4) | Failure 3 (no upsample) | −0.3 to −0.6 (measure) | Identical to full res | idea (whitepaper) | ~4-6 h | GPU timestamps; bit-exactness vs current at same pattern |
| 7 | Still-camera temporal accumulation with variance clip + depth rejection (§5) | Failure 4 | −1.2 (Low + history) | Beyond-Ultra when still; spatial AO when moving | papers | ~4-6 h | Lab shift; pan capture for trails |
| 8 | Half res + checkerboard min/max + nearest-depth upsample (§4) | Failure 3 (cheap) | −1.5 | Some foliage dots remain | papers | ~3 h | Lab shift <= 0.3 levels |
| 9 | Visibility bitmask (§6) | thin-object halos | +0.5-1 (emulated) | Less over-darkening | paper | ~1 day | Only on reports |

Ranks 1-4 can go to Apex as one PR each with lab proof (their AO lab `gtaolab.cpp` and the depth dumps); 6-9 only for a
GPU-bound target, matching 09's verdict. The game is CPU-bound on the render thread (profile §10), so GPU savings reach
frame time only on GPU-bound setups (laptop RTX 30 at 4K is one).

## Sources

- XeGTAO, Intel, MIT: https://github.com/GameTechDev/XeGTAO (shader `Source/Rendering/Shaders/XeGTAO.hlsli`: `XeGTAO_CalculateEdges`, `XeGTAO_CalculateNormal`, `XeGTAO_Denoise`; README: 1.4 ms High at 4K on RTX 3070, Hilbert/R2 noise replaced blue noise)
- Jimenez, Wu, Pesce, Jarabo 2016, GTAO: https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf
- Therrien et al. 2023, visibility bitmask: https://arxiv.org/abs/2301.11376
- ASSAO, Intel, MIT: https://github.com/GameTechDev/ASSAO and https://www.intel.com/content/www/us/en/developer/articles/technical/adaptive-screen-space-ambient-occlusion.html
- FidelityFX CACAO, AMD, MIT: https://github.com/GPUOpen-Effects/FidelityFX-CACAO
- HBAO+, NVIDIA source licence: https://github.com/NVIDIAGameWorks/HBAOPlus
- Deinterleaved texturing, NVIDIA: https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/d3d_samples/deinterleavedtexturingsample.htm
- Keller & Heidrich 2001, interleaved sampling: https://diglib.eg.org/handle/10.2312/EGWR.EGWR01.269-276
- McGuire et al. 2012, SAO: https://research.nvidia.com/sites/default/files/publications/McGuire12SAO.pdf
- Jansen & Bavoil 2011, nearest-depth upsampling: https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/OpacityMappingSDKWhitePaper.pdf
- Pettineo, nearest-depth upsampling in practice: https://therealmjp.github.io/posts/programmable-sample-points
- Turánszki 2019: https://wickedengine.net/2019/09/22/improved-normal-reconstruction-from-depth
- Wu 2021: https://atyuwen.github.io/posts/normal-reconstruction/
- Salvi 2016: https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf
- Karis 2014, "High Quality Temporal Supersampling", SIGGRAPH Advances in Real-Time Rendering (course page https://advances.realtimerendering.com/s2014/; not opened)
- STBN, NVIDIA (non-commercial licence): https://github.com/NVIDIA-RTX/STBN and https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks
- qUINT / MXAO (all rights reserved): https://github.com/martymcmodding/qUINT

Licences of HBAO+, STBN, ASSAO, XeGTAO, CACAO and qUINT checked through the GitHub API on 2026-11-17. Kopf 2007
(joint bilateral upsampling) and the Black Ops 3 checkerboard reference are cited from secondary sources, not opened.

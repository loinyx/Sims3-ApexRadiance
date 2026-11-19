# Ambient Occlusion: history

Chronological record of investigations and design decisions. The current behaviour is described in
[features/ambient-occlusion.md](../features/ambient-occlusion.md). Earlier AO implementations are in
[removed-features.md](../removed-features.md).

### 2026-09-28: verdict on the previous AO

**Context:** the combined build shipped HBAO at half resolution.

**Finding:** in game the shade looked dirty and too dark, or weak once toned down, with dots and bands indoors and
outdoors.

**Outcome:** the old AO was removed. Half resolution, temporal noise, near-unit radii and the plain multiply composite
are not to be reused.

### 2026-09-30: GTAO study

**Context:** AO was reintroduced as a standalone effect. A CPU lab (`gtaolab.cpp`) compared the shipped HBAO with a new GTAO on six
saved game frames (3840x2160 depth dumps plus colour).

**Finding:**

- A still camera gives bit-identical output; the shift metric (image moved 1 to 4 px) matches the old HBAO.
- Flat surfaces get no shade because visibility is a ratio of the unoccluded arc. The shaded share of an indoor frame
  fell from 64% to 46%, all of it where surfaces meet.
- The new composite (dead zone, multi-bounce, light protection) makes bright pixels lose 3.6 times less light than the
  old composite.
- The GPU shader matches the CPU lab within a fraction of a level. The fast `acos` fit was 1.3 levels off on 68% of
  pixels; exact `acos` costs 7 instruction slots.

**Outcome:** GTAO at full resolution with composite (d) shipped in 2.1.0. The Experimental badge was removed the same
day and replaced by a performance note.

### 2026-09-30: quality levels and cost

**Context:** measuring cost per quality level on scenes 1, 2 and 6 of the lab (`aotime.cpp`), native D3D9, RTX 4070 Ti
SUPER.

**Finding:** the original 2.1.0 High cost 3.9 to 4.3 ms, mostly trilinear R32F filtering (two bilinear lookups per tap,
64 taps per pixel). Switching to point mip filtering brought High to 3.0 to 3.2 ms with an image difference of 0.03 to
0.05 levels. Fewer steps (3) moved the look by 3 to 4 levels; box-only blur saved 0.1 ms but was less stable; coarser or
finer read levels, the fast `acos` and an R16F pyramid copy gave no gain.

| Level | Slices | Total ms | Image vs 2.1.0 High | Shift |
|---|---|---|---|---|
| 2.1.0 High (trilinear) | 8 | 3.9 to 4.3 | 0 | 0.28 to 0.51 |
| Very Low | 2 | 1.3 | 0.10 to 0.22 | 0.57 to 1.07 |
| Low | 4 | 1.8 to 2.0 | 0.08 to 0.17 | 0.39 to 0.73 |
| Medium | 6 | 2.5 to 2.65 | 0.07 to 0.13 | 0.33 to 0.59 |
| High | 8 | 3.0 to 3.2 | 0.03 to 0.05 | 0.28 to 0.50 |
| Ultra | 12 | 4.1 to 4.5 | 0.06 to 0.11 | 0.23 to 0.41 |

**Outcome:** Very Low and Ultra were added; stored indices keep 2.1.0 compatibility. Map view radii (contact 4 m, large
15 m) were set from three map captures (view 950 to 1800 m, AO shift 0.35 to 0.48 levels).

### 2026-09-30: planned ambient share (not implemented)

**Context:** AO darkens all light, including lamp light, which limits how strong it can be.

**Finding:** patched shaders could write the ambient share of each pixel to a second render target, so AO darkens only
ambient light. The back-buffer alpha cannot be used because bloom reads it.

**Outcome:** deferred. Order if resumed: interior walls, floors and furniture, then terrain, exterior walls, Sims and
foliage.

Sim-specific work continues in [sim-occlusion.md](sim-occlusion.md).

## 2.11.1: deterministic AO only

Temporal smoothing and visibility-bitmask detail are removed, including their shader variants, reprojection logic, history textures and settings. AO keeps the existing full-resolution horizon path, spatial blur and Sim controls. Legacy temporal/thinDetail/thickness keys no longer participate in settings registration or built-in profiles.

## 4.0.2: fewer directions when far and smooth flat surfaces (options, off)

**Context:** two dropped ideas came back as options. Slices by reach on screen (a count per pixel) looked blotchy because
neighbouring pixels used different direction sets, so the 4x4 box no longer cancelled the interleave. Smoother normals
(2 px central differences on "even" surfaces) drew dashed lines along mesh seams because the "even" test flipped pixel
by pixel.

**Finding:** one count per frame keeps one direction set in every 4x4 block; blended quadrant normals (XeGTAO's edge
weights, MIT) have no per-pixel decision to flip. Offline check (`tools/ao_options_check`, synthetic depth buffer, High,
1080p): the zoom option equals the default up close and differs by 0.12 to 0.13 of a level on average at 80 and 150 m,
with the open floor as smooth as before; blended normals cut the stray shade on the 24-bit open floor by about a third
(mean 0.29 to 0.20 levels at 40 m) and keep 96 to 97% of the contact shade. The shipped one-sided normal showed no
dashes on the synthetic 0.03 degree crease either. AO pass at 3840 x 2160 on a laptop RTX 30-series GPU (native D3D9,
noisy within about 10%): about 6.2 ms up close for both, 7.3 to 5.4 ms at 80 m and 8.4 to 3.8 ms at 150 m with the zoom option.

**Outcome:** both ship off. They need the in-game picture pair (`ao-adaptive-ab`) and the in-game GPU timer before a
default can change.

## 4.0.2: four more options (off): steady count, edge-aware smoothing, open surfaces, still-camera refinement

**Context:** follow-ups to the two options above, from the same prior-art review (XeGTAO and ASSAO, Intel, MIT; the
temporal anti-aliasing practice of clamping a history to the current frame's neighbourhood).

**Finding** (`tools/ao_options_check`, synthetic depth buffer, native D3D9 on a laptop RTX 30-series GPU with other
work on the GPU; times at 3840 x 2160 are noisy within about 10%):

- *Steady direction count*: with walls 2% either side of a threshold in turn the count switched 14 times in 16 frames;
  with a 10% band kept in two 1x1 targets it never switches, and still follows the view past the band.
- *Edge-aware smoothing*: the 4x4 box weighted by XeGTAO's slope-adjusted edge weights, without the tent, stays 0.14 to
  0.19 of a level from the default, leaves no interleave (open-floor roughness 0.007 to 0.008 against 0.005 to 0.006),
  keeps 100 to 104% of the contact shade and a 10 to 14% larger shade step at silhouettes. The blur passes take 0.31 ms
  against 0.57 ms.
- *Fewer directions in the open*: one count per 4x4 block, half where 1/z does not bend within reach. 67 to 78% of the
  blocks run at half the count, the image stays within 0.01 of a level, contact shade 100.0%, no extra shade step at
  block edges where the count changes. The AO pass takes 4.7 to 5.6 ms against 7.0 to 7.9 ms (High).
- *Refine while the camera is still* (Low): 0.08 of a level from Ultra against 0.15 for plain Low after 32 still frames;
  a removed object's shade is gone in one frame (0.00 levels left); 0.015 of a level of change between late frames on
  average, 0.05% of the pixels by more than 1 level, one pixel by up to 9. The AO and blur passes take 4.7 ms against
  4.4 ms for plain Low and 7.7 ms for High.
- The looped march at tier 0 gives the same image as the default pass (0.00 levels apart) in 357 instruction slots
  against 505, at 7.9 ms against 7.7 ms for the AO and blur passes, which is inside the noise.

**Outcome:** all four ship off. Each needs the in-game picture pair and the in-game GPU timer; the still-camera
refinement also needs a still view with moving Sims and leaves, because its residual flicker at thin outlines is new
compared with the deterministic pass. The default pass keeps its unrolled march until the loop is timed in game on
native D3D9 and DXVK.

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

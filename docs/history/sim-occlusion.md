# Sim Occlusion: history

Chronological record of investigations and design decisions. The current behaviour is described in
[features/sim-occlusion.md](../features/sim-occlusion.md).

### 2026-10-04: Sim coverage, black lips and mouth

**Context:** the coverage preview of the 12:58 session showed lips, mouth interior and small clothing gaps in black
(scene shade). No per-pixel capture existed for the exact material.

**Finding:** the receiver generator rejected every blended body material (`transparent && !hair`), although the draw
filter accepted them. The 13:51:55 F7 hit the cheek (SimSkin PS 2ED2423F / VS 5CB63E54, recognised), and its
visibility query showed two passing skin fragments, compatible with overlap. A replay also lost the one-pixel scissor
rectangle, which inflated the eyelash query count.

**Outcome:** the restriction was removed and tests now generate blended body shaders. The scissor rectangle is
reapplied after binding render targets. The first correction passed 9,388 shader checks and 66 replay checks.

### 2026-10-04: overlapping triangles and depth comparison

**Context:** a near triangle followed by a farther one in the same draw left the receiver at the farther depth while
the colour draw kept the nearer one.

**Finding:** the receiver callback runs before the original draw, so replays must keep the game's exact comparison.
Widening LESS to LESSEQUAL marks coplanar Sim fragments that the colour draw rejects over a visible non-Sim surface.

**Outcome:** receivers moved to G32R32F with MIN blending, keeping the nearest depth in either order. Opaque replays
accept LESS and LESSEQUAL only; other modes keep the scene shade. Colour-disabled draws are excluded. Final run: 9,400
shader checks and 223 replay checks on native D3D9 and DXVK 3.1.1. The reported scenario was confirmed in
game, and the Experimental badge was removed from the Sim Occlusion card.

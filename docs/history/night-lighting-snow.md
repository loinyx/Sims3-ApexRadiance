# Night Lighting, snow: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/snow.md](../features/night-lighting/snow.md).

### 2026-09-25: snow formula analysis

**Context:** a strong cut at the lot border in snow (m01/m02, `LightProbe-neve`); captures m04 (grey lot), m05 (bright
world) and m06 (road).

**Finding:** the lot light pass multiplies the lot map by 0.25 (`def c9.x`), so the lot term is four times weaker than
the world term, and the lot map has no street lamps.

**Outcome:** lot = `max(lot x factor, terrain x 4)`; road = extra `texld` of the terrain map + `max`
([roads.md](../features/night-lighting/roads.md)).

### 2026-09-25 (about 01:40): snow fix list

**Context:** the pending list of winter defects.

**Finding and outcome per item:**

| # | Item | Outcome |
|---|---|---|
| 1 | Pink lamps still pink (56 street lamps type 11, 46 lot lamps type 3 at 1/0.75/0.79; the script sets the colour after creation via `FUN_006B0B50` -> `FUN_006BC3E0`) | Fixed about 02:10 by redirecting the call at `0x006B0BDE` ([lamp-colour.md](../features/night-lighting/lamp-colour.md)) |
| 2 | Dark strip at the road edge (screenshot 41; m24, fourth road variant) | Fixed by the generic `PatchRoad` |
| 3 | Dark squares at sidewalk corners (m16/m18, third road variant) | Fixed the same way |
| 4 | Black pond with white streaks in the snowy plaza (screenshot 42; m25): MSAA on, no INTZ, the 40 m guess sampled the scene copy wrongly | Fixed: no screen reflection without depth ([water.md](../features/night-lighting/water.md)) |
| 5 | Winter bushes dark (m21: moon shadow multiplies lamp light; m22: back side zeroed by `max r0, r0, c132.w` in the VS) | Fixed: `PatchLeafShadow`, `PatchFoliageVs` ([foliage.md](../features/night-lighting/foliage.md)) |
| 5b | Snowy tree (m28, `PS_2A83D588` / `VS_2A839190`, no shadow map) | Wrap light via `PatchFoliageVs` |
| 6 | Snowy roofs "waiting" (m23, `PS_2793C3E8` 4992 bytes, `VS_27944BD8`) | Fixed: additive pass `roof_snow_lamps_ps.hlsl`; m29 position fix ([roofs.md](../features/night-lighting/roofs.md)) |
| 7 | Snowy floor (m08, `PS_1B3938E8`, lot map only x 0.25) | Fixed: world atlas + `PatchFloor` |

Later additions the same day: snow on fence tops (m48, about 10:01), stair snow (m50/m51, about 10:40), snow on floors
around the pool (m69), door sills (m71/m72), pool edge (m66).

### 2026-09-25 (10:43): stairs sent to the roof-snow handler

**Context:** first test of stair snow.

**Finding:** the stair snow pixel shader is byte-identical to the snowy roof one, and the PS class was tested before the
VS classes, so stairs went to `DrawRoofSnow`, which computes lamp positions from roof VS constants.

**Outcome:** `IsSnowReliefVs` (class 9) and the routing rule. Lesson: classify by vertex shader before trusting a pixel
shader class shared by two families. `PatchSnowCover` could not be reused because it refuses flow control and stair snow
has four `rep` loops; a separate patch inserts outside the loops.

### 2026-09-25 (about 14:35 to 15:00): snow on floors never ran

**Context:** review of the snow-floor fix.

**Finding:**

- The m69/m71 pixel shaders declare `s6` and up and fell into the world-candidate branch, which returned the draw to the
  game, so `DrawSnowFloor` never ran.
- `IsSnowFloorVs` caught terrain and water vertex shaders; the A8R8G8B8 check did not separate lot maps, which are also
  A8R8G8B8.
- The tc 0 and tc 7 variants of the same pixel shader need separate patched copies.

**Outcome:** `DrawSnowFloor` is called when `RecordWorldChunk` rejects the draw; vertex shaders with the terrain-UV `mad`
are refused and the replicated 0.5 is required; separate caches per tc.

### 2026-09-25: winter seam at a lot border, m73, m74, m76 (date approximate)

**Context:** a bright snowy lot (PS 33A013F8, m73) next to dark winter world terrain (PS_2FA80C68, 2488 bytes, smoothed
map in `s9`, m74).

**Finding:** the dark side read chunk (896, 896), the lot used centre (640, 896), border at x = 768. Hypothesis 1 (a stale
smoothed map after an unreadable game map) and hypothesis 2 (lots reading the home chunk with CLAMP, confirmed by m76)
both applied.

**Outcome:** both fixed; lots read the world atlas ([world-atlas-and-smoothed-maps.md](../features/night-lighting/world-atlas-and-smoothed-maps.md)).

### 2026-09-25: plans not implemented

**Context:** open items.

**Finding:**

- VS 436BB272 lot snow variant (needs its own constant mapping: `c15` centre, `c14` UV; seen only in m58, a town hall
  lot, draw #378).
- PASSO3 later increment 4: winter floors (`PatchFloor`) and VS 82e79a9e / PS 68113aad (m29) for a per-pixel term.

**Outcome:** not started.

### 2026-09-29: floor lamp scale constant (date approximate)

**Context:** in the combined build `FloorPatch::scaleConst` (`LampScaleAfter`) was added after v0.1.0 for the HDR gain
only.

**Finding:** HDR was removed from Apex Radiance.

**Outcome:** since 1.5.0 the constant carries *Ground brightness* on snow on floors.

### 2026-10-04: Ground brightness across squared terrain variants (PR #2)

**Context:** an F7 capture showed a world terrain lamp-map path using `c4.x x c4.x` through a scalar temporary, next to
the linear `c7.x` path. Only the linear path received *Ground brightness*, which exposed a boundary when the gain was not
100%.

**Finding:** the squared multiplier can be identified exactly from bytecode (one temporary, `cK` read exactly twice, no
modifiers); the runtime already had the square-root compensation for the multi-pass shader.

**Outcome:** `LightMapScaleConst` reports `squared`, and `TerrainConst` applies the square-root form. Commit `622a93c`.
Visual validation on snow and grass pending.

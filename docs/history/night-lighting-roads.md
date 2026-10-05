# Night Lighting, roads and sidewalks: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/roads.md](../features/night-lighting/roads.md).

### 2026-09-25: snowy road analysis

**Context:** in snow, lamp-lit ground next to a road showed a dark mesh on top (captures m01/m02, `LightProbe-neve`).

**Finding:**

| Capture | What it showed |
|---|---|
| m01 / m02 (snow, dark vs lit ground) | The dark side had a mesh on top (#108, PS_26B051F8, 2667 prims, road and sidewalk) with its own light map in `s6` |
| m04 / m05 / m06 (snow formula analysis) | Road PS_27A1AD10 (same in m01 and m06) uses a copy of the chunk map in `s6` (texture T14, not the terrain's T12); UV = local/256 + 0.5 (VS `c16`); chunk centre = world translation (VS `c8.w`, `c10.w`). The copy has no lamp glow |
| m16 / m18 (`LightProbe-rua-canto`) | Third winter variant, sidewalk corner |
| m24 (`LightProbe-rua-beirada`) | Fourth winter variant, road edge blended into the terrain; stencil passes before it (#118/#119, no PS) |
| Screenshots 41, 43 | Dark strip at the road edge in snow; dark squares at sidewalk corners |

In variant 1 with snow, the bright parts of the road texture (sidewalk, markings) always become snow texture.

**Outcome:** `max(road map, chunk terrain map)` on a free sampler, plus the luma-based sidewalk blend ("trodden snow").

### 2026-09-25 (about 01:40): byte-matched first version

**Context:** the first road fix matched only the two known winter pixel shaders by their bytes.

**Finding:** variants 3 (corner, already uses `r6`, so the temp must be configurable) and 4 (edge) stayed dark.

**Outcome:** rejected. Recognition moved to the vertex shader pattern, with the extra sampler and temp computed per
shader. Winter variants 1 to 4 tested offline and in game at about 02:10.

### 2026-09-25 (about 09:10): summer roads

**Context:** a lot lamp lit the grass through the terrain map (lot map there about 0, maximum 0.012) but not the road or
sidewalk (captures m42 `calcada-verao`, m43 `rua-verao`, `grama-luz-lote`).

**Finding:** the summer road VS keeps the map mapping in `c14`, and its pixel shader applies the lamp scale a few
instructions after the `texld`, so it was not recognised.

**Outcome:** `IsRoadVs` stores the mapping constant per shader; the lamp-scale search was widened to 8 instructions,
stopping on any other read or RGB overwrite of the map register.

### 2026-09-25: open idea, sharper terrain stamp

**Context:** PASSO3 plan, increment 5.

**Finding:** a stamp at 4 texels per metre (PASSO3 increment 5) would also sharpen roads, which read the same map.

**Outcome:** not started.

### 2026-09-25: road partition marking on full rebuilds

**Context:** in the combined build a full terrain rebuild also marked the road partition per chunk (`FUN_00B789B0`,
`WorldManager+0x5C` = RoadNetwork) from `smooth_streaming_patch.cpp`.

**Finding:** the marking belonged to the streaming work, not to the road lighting.

**Outcome:** removed with Smooth Streaming (see [removed-features.md](../removed-features.md)); not in Apex Radiance.

### 2026-09-29: road scale reused for brightness (1.5.0)

**Context:** in the combined build `RoadPatch.scaleConst` (`LampScaleAfter`) carried only the HDR lamp gain.

**Finding:** the constant is the road's own lamp scale, read once, so it can carry any per-draw gain.

**Outcome:** HDR was removed; since 1.5.0 the same constant carries the *Roads and sidewalks* and *Ground brightness*
gains.

### 2026-10-03: alpha-blended sidewalk variant (2.5.6)

**Context:** a sidewalk variant packs the terrain UV in TEXCOORD1.xy and the opacity UV in TEXCOORD1.zw (full mask), so
`IsRoadVs` refused it.

**Finding:** an offline scan of 314 captured vertex shaders kept the three existing road matches and added one
alpha-blended sidewalk match.

**Outcome:** the `.xyzw` form is accepted only with the validated opacity-UV `mul`, unit-scale `def` and
texture-coordinate input. Released in 2.5.6. An earlier note that the scale constant was HDR-only was superseded.

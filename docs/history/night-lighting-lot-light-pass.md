# Lot light pass: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/lot-light-pass.md](../features/night-lighting/lot-light-pass.md).

### 2026-09-24: failed attempts recorded in the notes (section 1, "do not repeat")

**Context:** the straight cut of street-lamp light at lot borders.

**Finding:** changing the world light collection radius: no effect. Re-solving room 0: no effect (kept as "Refresh
lots" and `recalcularLotesAoAnoitecer`). Rebuilding the type-5 terrain textures: no effect. High quality on every lot:
no fix, and FPS dropped to 63 (kept as `qualidadeAltaEmTodosOsLotes`). Turning off the terrain texture in the lot
layer (stub at 0xC7F87D): no fix (kept as `gramaDoLoteUsaLuzDoLote`). Counting street lamps as lit in the solve: no fix
alone (kept as `postesAcesosNoCalculo`). Section 1c: the bridge can keep an older DXT5 chunk map in `g_chunks` if the
world draw still binds it; not visible in practice. `LightProbe-grama3-escura`: the "dark" lot grass was a different
terrain paint, not a lighting bug: check albedo before blaming light.

**Outcome:** the replacement lot pass with `max(lot map, terrain)`. Confirmed in game.

### 2026-09-24: sampler variants and chunk seams

**Context:** lots on some chunks were not fixed; a cut appeared on world grass itself.

**Finding:** the world terrain PS reads the light map from a sampler that depends on the number of paint layers: s8
with 4 layers, s7 with 3 (`PS_294418E0`, notes 1b), s6 (summer 86B88B85/1420), winter s10..s12 (ground_report.md A). The
first bridge looked only at s8 (`LightProbe-grama2`, `grama2-claro`). Separately, `LightProbe-grama2` showed a cut at
x = 1280 between chunks centred (1408, 1152) and (1152, 1152): the lamp circle existed only in the chunk that owns the
lamp, a defect of the map baked into the world file. The game's own rebuild includes every light whose range touches
the chunk.

**Outcome:** the scan covers s1..s15 and any PS declaring s6+ is a candidate; a rebuild after loading fixes the seam;
the smoothed maps read 6 texels from each neighbour and lots read the atlas, so no seam comes from the mod.

### 2026-09-25: lot sampling past its home chunk (m76)

**Context:** a lot at z 1290 on the chunk that ends at z 1280.

**Finding:** sampling the home chunk map with CLAMP stretched the chunk's last row: dark lot with a straight edge next
to a lit sidewalk.

**Outcome:** lots read the atlas. Do not return to home-chunk sampling when the atlas is available.

### 2026-09-28: soft lot edges

**Context:** `research\borda2`, `research\borda3`: a street lamp (#1929, type 11, head (958.4, 61.8, 1199)) 0.7 m inside
the edge of lot 09080020A1D28860 (lot-local (29.3, 11.5); edge at lot-local x = 30).

**Finding:** the lot map (s1, 256x128 A8R8G8B8, 3.94 texels/m) saturates at 1.0 within about 2.5 m of the lamp; the
atlas (1 texel/m stamp, smoothed) peaks at R 0.89 / G 0.69 there. So `max(lot, atlas)` is 1.0 just inside the edge
and the world grass shows the atlas just outside. Beyond about 3.5 m inside the atlas already wins (the terms cross at
about 4 m). Capture numbers (borda3/clara, T6 lot map and T7 atlas 2048x1536 with c14 = (1/1024, 1/768, 0.375, 0.5),
bilinear, along lot-local z = 11.46, W = 30 as the lot map content ends at texel 119 = 3.9375 x 30 + 1):

| lot-local x | from edge | lot R | atlas R | before (max) | soft edges |
|---|---|---|---|---|---|
| 27.0 | 3.0 in | 0.974 | 0.644 | 0.974 | 0.974 |
| 28.0 | 2.0 in | 1.000 | 0.777 | 1.000 | 0.942 |
| 29.0 | 1.0 in | 1.000 | 0.879 | 1.000 | 0.911 |
| 29.75 | 0.25 in | 1.000 | 0.886 | 1.000 | 0.889 |
| 30.0 | edge | 1.000 | 0.876 | 1.000 | 0.876 |
| 30.25 | 0.25 out | - | 0.862 | 0.862 | 0.862 |
| 31.0 | 1.0 out | - | 0.803 | 0.803 | 0.803 |

G at the edge: 1.000 before, 0.683 after, 0.672 at 0.25 m outside. The step was +14% (R) and +46% (G) of the lamp term;
after the fix both sides are the same atlas sample (0.25 m apart they differ by 1.6% R / 1.6% G, the atlas' own
slope). The two probe pixels map (lot pass VS c4..c7, ground plane) to lot-local (29.68, 10.86) = 0.32 m inside
(borda3/clara, screen (0.275, 0.333, 0.039)) and (30.48, 11.22) = 0.48 m outside (borda3/escura, (0.259, 0.267, 0.027)).
uv (0.435249, 0.561012) -> (29.6800, 10.8600) in float32. LightDiag's `matriz[+0xF8]` of the lot,
(-0.3746 0 -0.9272 0 | 0 1 0 0 | 0.9272 0 -0.3746 0 | 958.7 60.52 1230 1), equals the lot pass VS
c8 = (-0.3746, 0, 0.9272, 958.74), c10 = (-0.9272, 0, -0.3746, 1230.44) (answers PASSO3 question 3 for rotation and
translation). Capture id of the compiled replacement before soft edges: F688FB46/1020 (MD5 prefix / size).

Rejected designs: (a) the lot map uv rect cannot give the lot size: the texture is sized `nextPow2(4 x size)` by
`FUN_006a8de0` and holds more than the ground (256 wide for a 30 m lot, content ends at texel 119), and the VS lot-map
formula `(v0 x 63/128 + 0.25) x c12` uses `def` constants the CPU cannot read; (b) the lot map has no coverage signal:
room-0 texels have alpha 0 and outside texels are black, like any unlit texel (T6 dump), so a content bounding box would
fade porch-lamp pools in the middle of a lot; vertex-buffer bounds of the lot pass: tiles under floors may be missing
and the VB pool is unknown.

**Outcome:** design (c): lot rectangle from room 0 and the VS lot matrix, 3 m smoothstep feather. Kept.

### 2026-09-29: leftover samplers taken as a chunk's map

**Context:** a gameplay video showed road, fence and lot fixes switching on and off with the camera.

**Finding:** the chunk-map scan looked at samplers above those the PS declares, so a 256x256 map left bound by an
earlier draw (for example the neighbour chunk's map in s8 while a 3-layer chunk reads s7) was taken as this chunk's map,
depending on draw order. Each flip also re-smoothed the chunk.

**Outcome:** `Classify` records the samplers every `WorldCandidate` declares and the scan looks only there; a texture
already registered for another chunk (`g_chunkOfTexture`) is skipped and counted ("leftover textures skipped when
looking for chunk light maps"). `RecordWorldChunk` returns the `g_chunks` entry.

### 2026-10-02: continuity regression investigation

**Context:** street-lamp light discontinuity across the lot/world boundary, probes 01:08:51 (world) and 01:08:54 (lot).

**Finding:** reconstruction on the lot ground plane puts the lot probe about 0.46 m inside x = 30 and the world probe
about 0.48 m outside, at different positions along the edge. The replacement was present, with the 30x40 rotated lot
transform, the 3 m feather and the atlas. Offline, the dumped atlas and the world smoothed texture at matching
coordinates differed by about one 8-bit level or less in red: no black or missing cell, but no proof of final rendered
continuity (paint layers, composition, ground reconstruction). The tested type-11 lamp was disabled (flags 0x35) while
both maps kept its red stamp. Captures 01:25:58 and 01:26:01 (both world terrain; the second the wrong, brighter one)
then exposed the unsupported summer multi-pass world shader ([world atlas history](night-lighting-world-atlas-and-smoothed-maps.md)).

**Outcome:** the multi-pass correction resolved the reported cutoff (see validation). The feather shader or queue tests
alone are not evidence of continuity. Terrain pacing and smoothing optimisations were kept.

### 2026-10-02: visible-lot response

**Context:** lamp edits on newly visible lots reached the ground late.

**Finding:** arrival detection needs a reliable "lot is on screen" signal.

**Outcome:** the verified regular lot pass records the matched lot's visibility even with soft edges off, using the
manager lot id and the verified vertex-matrix rectangle, without changing shader output. Rules in
[terrain relight](../features/night-lighting/terrain-relight.md); unverified lot variants keep the previous fallback.

### 2026-10-04: daylight term and state review (PR #2)

**Context:** daytime Build preview showed world terrain and lot grass with different lamp terms
([world atlas history](night-lighting-world-atlas-and-smoothed-maps.md)).

**Finding:** the world shaders' lamp RGB factor is zero by day; the lot replacement needed the same daylight term on
its terrain part. Review: the regular and snow lot paths computed an unused atlas mapping from uninitialised floats when
smoothing was off or the atlas not ready; the regular pass restored c28..c31 after a read that could have failed.

**Outcome:** c31.y = `DayLampScale`; atlas coordinates zero-initialised; the draw is left to the game when the c28..c31
read fails. Draw math and texture selection unchanged otherwise.

### 2026-10-04: lot UV contraction (PR #2)

**Context:** a ground capture (F7 18:35) during the indoor wall guard work showed lot grass outside a closed wall
sampling the indoor row of the lot map.

**Finding:** the regular lot VS (`kLotLightVs`) maps local xz as `(xz x 63/64 + 1/4) x c12`; at local 33.1249 m the
contracted texel (`(local x 63/64 + 0.25) x 4 - 0.5`) stays below 131 while the aligned one is above 131.99.

**Outcome:** the exact VS gets the inverse mapping through c31.zw; other layouts keep native coordinates. Candidate
`2.5.6-indoor-wall-guard-test`.

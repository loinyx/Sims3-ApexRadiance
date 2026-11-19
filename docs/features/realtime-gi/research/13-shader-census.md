# 13. Shader census for the light-map carrier (S0-04)

Work package S0-04. Question: can Apex-owned light-map copies of another size, format or content (scale, float,
four directional maps) replace the game's maps without patching shaders? Source: `Shaders_Win32.precomp` (Steam
1.67), read-only, with `research/tools/precomp.cjs` (mode `census`, 49,631 passes, 8,287 distinct shader pairs) and
`research/tools/census-report.cjs` (makes the table). Spot checks with `fxc /dumpbin` on the extracted bytecode.

Tags: **verified** = decoded from the shader bytecode of every pass in the census; **inferred** = reasoned, not
measured; **open** = not answered here.

## 1. Findings

1. **`LightmapSizeParameters` is read only by vertex shaders, never by a pixel shader (verified, 0 of 49,631
   passes bind it in the pixel stage).** Every read is a `mul` of a texel or lot coordinate by the constant into a
   texture-coordinate output, for example Counters `mul o2.xy, r3.zwzw, c198.zwzw` (swizzle `zw`) and ExteriorFloors
   `mul o3, ..., c11` (full vector). It is a uv scale, not a texel offset: **no shader uses it for texel offsets**
   (closes 02 §3.2 point 2).
2. **No pixel shader adds an offset to a light-map uv (verified).** In all 6,724 light-map samples the coordinate is a
   direct interpolant (or, in 640 `ps_2_0` samples, a `mov` of one). There is no hand-made filter, so a larger or
   float copy bound in the same sampler slot is read by the same bilinear fetch.
3. **Walls bake the scale into the vertex shader**: InteriorWall `mul o2.xy, c11.z, v3` with `def c11.z =
   0.000244140625` (1/4096). Floors, terrain, snow and pond multiply by `LightmapSizeParameters`. Objects
   (Counters, SingleObject, InstancedObject) multiply a lot-space position by `.zw`; Phong, Rug, Painting and the
   other `LightMapXform` families do not read it (the matrix carries the scale). All of these give a normalised uv, so
   **a copy with N times the texels over the same domain needs no shader change** (inferred from the above; the
   constant's value is set by the engine at `0x006A4700` and was not read here, open).
4. **Directional maps use fixed weights in the pixel shader (verified).** The four directions are `def` constants,
   never set by the game: `(+-0.8944, 0.4472)` in the x/y plane and `(0.4472, +-0.8944)` in the y/z plane, that is
   unit vectors tilted about 26.6 degrees up from the horizontal. Order (verified on 408 passes with a clean decode and by
   assembly on samples, the rest are tracer limits): `LightBasisMap0 = +X`, `1 = -X`, `2 = +Z`, `3 = -Z`.
5. **Two readers of the four maps.**
   - **Objects (Counters, SingleObject, InstancedObject):** colour of map i times `sat(N . d_i)`, `N` from the pixel
     normal, then `max` with the light map and sky (02 §1.2). A copy must hold per-direction irradiance (the lobe
     weight is applied by the shader).
   - **Floors and terrain (InteriorFloor, Hideable, FloorWith*, ExteriorFloors, TerrainLight*, SnowFloorEdges):** each
     map is reduced to luminance (`dp3` with a `def` 1/3 weight), summed along the four direction vectors into a
     dominant light direction, then `N . L` with the normal map. A copy must keep **relative luminance per direction**;
     a map with no directional contrast gives a flat floor.
6. **Highest constants read by light-map readers:** pixel shader `c23` (TerrainHigh, `c22` ExteriorFloors, `c17` Phong,
   Road); vertex shader `c211` (skinned Phong and objects, indexed palette) and `c24` for the lot floors and terrain.
   The rest of the pixel range is free, as in 02 §1.4 (the bound pixel table also ends at `c23`).
7. **Families that must stay at scale 1: none, from the shaders (inferred).** The conditions that remain are not in
   the shaders: (a) the copy covers exactly the original domain with the original uv origin, (b) atlas strips keep
   their gutters in proportion (S1-05, strip-edge seams), (c) a family whose vertex shader multiplies by the one
   shared `LightmapSizeParameters` must be bound to a copy of the same domain as the map that value was made for,
   or the constant must be set per draw (Apex's smooth-read copies already do).

## 2. Table of families

Passes counted per technique (a technique repeats many passes); "direct" = number of light-map `texld` whose
coordinate is an interpolant; highest constants are the highest register the shader reads that is not a `def`.

| Family | Passes (light-map / basis) | LSP in VS (reg: reads) | LSP in PS | Light-map uv in PS | Basis weights | PS ver (2/3) | Highest PS const read | Highest VS const read (bound) |
|---|---|---|---|---|---|---|---|---|
| AOandSI | 1 / 0 | c4: mul.xyzw (1) | none | 1 direct | - | 1/0 | - | c4 (c4) |
| BrushedMetal | 4 / 0 | - | none | 4 direct | - | 0/4 | c13 | c204 (c204) |
| BurntTile | 2 / 0 | c4: mul.xyzw (2) | none | 2 direct | - | 2/0 | c1 | c4 (c4) |
| Ceiling | 2 / 0 | - | none | 2 direct | - | 2/0 | c0 | c4 (c4) |
| Counters | 304 / 304 | c16,c18,c196,c198: mul.zwzw (384) | none | 256 direct | N.d per map | 112/144 | c10 | c210 (c210) |
| ExteriorFloors | 7776 / 1920 | c8,c11,c12,c15: mul.xyzw (7776) | none | 1048 direct | luminance -> dominant direction | 448/600 | c22 | c24 (c24) |
| ExteriorWall | 162 / 0 | - | none | 58 direct | - | 31/27 | c10 | c20 (c20) |
| ExteriorWallAOSI | 162 / 0 | - | none | 38 direct | - | 20/18 | - | c20 (c20) |
| FloorTileCeiling | 7848 / 0 | c8,c11,c12,c15: mul.xyzw (7776) | none | 498 direct | - | 492/6 | c6 | c24 (c24) |
| FloorWithBlacklight | 72 / 12 | - | none | 40 direct | luminance -> dominant direction | 16/24 | c11 | c12 (c12) |
| FloorWithStrobelight | 72 / 12 | - | none | 40 direct | luminance -> dominant direction | 16/24 | c12 | c12 (c12) |
| FloorWithStrobelightAndBlacklight | 72 / 12 | - | none | 40 direct | luminance -> dominant direction | 16/24 | c11 | c12 (c12) |
| Foliage | 4 / 0 | - | none | 4 direct | - | 4/0 | c13 | c204 (c204) |
| FullBright | 1 / 0 | - | none | 1 direct | - | 1/0 | c0 | c7 (c7) |
| Hideable | 74 / 12 | - | none | 42 direct | luminance -> dominant direction | 18/24 | c13 | c12 (c12) |
| InitialPaintsCausticsWithCutout | 0 / 0 | c7: mad.xyzw, mul.xyzw (2) | none | - | - | 0/0 | - | - (c-1) |
| InstancedObject | 768 / 768 | c8,c12,c13,c15,c17,c19,c88,c92,c93,c95,c97,c99: mul.zwzw (1824) | none | 624 direct | N.d per map | 264/360 | c6 | c109 (c109) |
| InteriorAOSIPlaceholder | 0 / 0 | c4: mul.xyzw (1) | none | - | - | 0/0 | - | - (c-1) |
| InteriorFloor | 80 / 12 | - | none | 48 direct | luminance -> dominant direction | 16/32 | c12 | c12 (c12) |
| InteriorWall | 60 / 0 | - | none | 24 direct | - | 16/8 | c11 | c13 (c13) |
| InteriorWallWithBlacklight | 48 / 0 | - | none | 16 direct | - | 16/0 | c10 | c13 (c13) |
| InteriorWallWithStrobeAndBlacklight | 48 / 0 | - | none | 16 direct | - | 16/0 | c10 | c13 (c13) |
| InteriorWallWithStrobelight | 48 / 0 | - | none | 16 direct | - | 16/0 | c11 | c13 (c13) |
| LeavesTile | 2 / 0 | c4: mul.xyzw (2) | none | 2 direct | - | 2/0 | c1 | c4 (c4) |
| LotImposter | 0 / 0 | c7: mul.xyzw (4) | none | - | - | 0/0 | - | - (c-1) |
| Painting | 4 / 0 | - | none | 4 direct | - | 4/0 | c12 | c200 (c200) |
| Phong | 544 / 0 | - | none | 360 direct | - | 120/240 | c17 | c211 (c211) |
| PhongAlpha | 320 / 0 | - | none | 136 direct | - | 56/80 | c16 | c211 (c211) |
| PondWater | 12 / 0 | c9,c13: mul.xyzw (18) | none | 9 direct | - | 3/6 | c10 | c24 (c24) |
| Road | 13824 / 0 | - | none | 1122 direct | - | 570/552 | c17 | c16 (c16) |
| RoadGrateDrain | 160 / 0 | - | none | 112 direct | - | 112/0 | c15 | c209 (c209) |
| Rug | 12 / 0 | - | none | 10 direct | - | 10/0 | c10 | c14 (c14) |
| SingleObject | 768 / 768 | c11,c15,c16,c18,c20,c22,c91,c95,c96,c98,c100,c102: mul.zwzw (1824) | none | 624 direct | N.d per map | 264/360 | c6 | c112 (c112) |
| SnowFences | 0 / 0 | c9,c13: mul.xyzw (6) | none | - | - | 0/0 | - | - (c-1) |
| SnowFloorEdges | 36 / 12 | c9,c10,c13,c14,c17,c18: mul.xyzw (48) | none | 26 direct | luminance -> dominant direction | 14/12 | c5 | c19 (c19) |
| TerrainHigh | 4032 / 0 | - | none | 624 direct | - | 96/528 | c23 | c22 (c22) |
| TerrainHighNoSnowElev | 64 / 0 | c10: mul.xyzw (64) | none | 2 direct | - | 2/0 | c3 | c14 (c14) |
| TerrainLight | 1728 / 192 | c7,c8,c11,c12: mul.xyzw (864) | none | 170 direct | luminance -> dominant direction | 80/90 | c9 | c19 (c19) |
| TerrainLightAndFog | 1728 / 192 | c7,c8,c11,c12: mul.xyzw (864) | none | 170 direct | luminance -> dominant direction | 80/90 | c9 | c19 (c19) |
| TerrainLightNoSnowElev | 1728 / 192 | c7,c8,c11,c12: mul.xyzw (864) | none | 156 direct | luminance -> dominant direction | 66/90 | c9 | c19 (c19) |
| TerrainLight_Indoor | 864 / 0 | c7,c8,c11,c12: mul.xyzw (864) | none | 18 direct | - | 10/8 | c1 | c18 (c18) |
| TerrainLight_IndoorWithStrobeLight | 864 / 0 | c7,c8,c11,c12: mul.xyzw (864) | none | 18 direct | - | 10/8 | c2 | c18 (c18) |
| TerrainLight_InsideGreenhouse | 1728 / 0 | c7,c8,c11,c12: mul.xyzw (864) | none | 84 direct | - | 66/18 | c5 | c19 (c19) |
| TerrainLow | 928 / 0 | c10: mul.xyzw (64) | none | 162 direct | - | 90/72 | c18 | c24 (c24) |
| billboard | 192 / 0 | - | none | 48 direct | - | 16/32 | c13 | c52 (c52) |
| billboard_alpha | 192 / 0 | - | none | 48 direct | - | 16/32 | c13 | c52 (c52) |
| fadeDownPlaceholder | 0 / 0 | c4: mul.xyzw (1) | none | - | - | 0/0 | - | - (c-1) |
| terrainAOSI | 4 / 0 | c7: mul.xyzw (4) | none | 1 direct | - | 1/0 | - | c11 (c11) |
| unlit | 0 / 0 | c4: mul.xyzw (1) | none | - | - | 0/0 | - | - (c-1) |
| unlitLotTerrain | 0 / 0 | c10: mul.xyzw (64) | none | - | - | 0/0 | - | - (c-1) |

TOTAL passes in census 49631 shader pairs 8287
PS-side LSP reads 0 ; computed light-map uv 0 ; direct 6724
light-map readers: highest PS const 23  highest VS const read 211  PS bound max 23  VS bound max 211

## 3. Method and limits

- Pass layout (verified): after `PASS` come `MRAP` blocks (first VS, second PS): `MRAP, id, count, count x (name,
  w1, w2)`, with `w1 = count<<24 | register<<14 | flags`. The register decoded from `w1` agrees with the registers the
  decoded shaders read (ExteriorFloors, Counters).
- Direction-to-map decode finds the nearest `dp2add` feeding the weight. It resolves 408 passes cleanly, about 1,000
  partially (normal-mapped variants reuse temporaries and separate `def` pairs); no decode contradicted the order in
  finding 4 except where the tracer followed the wrong temporary (checked on assembly of one example).
- `ps_2_0` shaders are included; `texld` with `texldl` is handled. Dynamic flow control does not occur in these
  readers (`flow = 0` in 02).
- Not covered: the value `0x006A4700` writes into `LightmapSizeParameters` (open, needs the setter), and the
  terrain-stamp `terrainLightMap` families, which are not room maps.

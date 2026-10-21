# Roads and sidewalks

At night, roads and sidewalks next to street lamps and lot lamps are lit as brightly as the grass beside them, in summer
and in winter. There is no dark strip along the road edge and no dark square at sidewalk corners. In snow, an optional
setting lets the sidewalk concrete show through where Sims have walked. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0. Alpha-blended sidewalk variant: released in 2.5.6 |
| Default | On (with Night Lighting and *Street lamps light lots*) |
| Menu | Lighting > Ground > *Ground intensity* (brightness); World > Water & Snow > *Snow* (sidewalk visibility) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`IsRoadVs`, `PatchRoad`), [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawRoad`) |

## The problem

Road and sidewalk meshes do not read the world terrain light map. Each road shader samples its own copy of the chunk's
256x256 light map, and that copy does not contain the lamp light that the terrain map has. Next to lit grass the road
stays dark. In winter the game's road shader also turns every bright part of the road texture (concrete, markings) into
snow, so sidewalks disappear under a uniform white cover. The terrain light maps are described in
[engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md).

## How Apex Radiance solves it

Apex Radiance recognises every road vertex shader by its instruction pattern and patches any pixel shader drawn with it.
The patched shader takes the brighter of the road's own map and the chunk terrain map, so roads receive the same lamp
light as the ground next to them.

1. **Recognise the road vertex shader** (summer and winter) from the way it computes the terrain UV.
2. **Patch the pixel shader** once per game shader: an extra `texld` of the chunk terrain map on a free sampler and a
   `max` with the road's own map, before the game's lamp scale.
3. **Bind the maps per draw:** the chunk terrain map on the new sampler and, when the smoothed map is ready, the smoothed
   map also in place of the road's own copy, so roads get exactly the clean light of the ground beside them.
4. **Sidewalk snow:** where the shader has the snow blend, mix the plain road texture back in on its bright parts by the
   *Sidewalk visibility* amount.
5. **Brightness:** scale the road's lamp constant by *Ground brightness* x *Roads and sidewalks*, weighted by the night
   level.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Roads and sidewalks (Ground intensity card) | `brilhoNasRuas` | float | 100% | 25 to 300% | Road lamp brightness relative to the ground; multiplied by *Ground brightness* (`brilhoNoChao`). Needs *Street lamps light lots* |
| Sidewalk visibility (Snow card) | `calcadaComNevePisada` | float | 50% | 0 to 100% | How much sidewalk concrete shows through snow; 0% keeps the game's look. Needs *Street lamps light lots* (`SetSidewalkClear` clamps 0 to 1) |

Roads also depend on *Street lamps light lots* (`luzDoPosteNaGramaDoLote`, the dispatch gate) and use *Smooth ground
light* (`mapaDeLuzSuavizado`) when it is on. Both are documented in [lot-light-pass.md](lot-light-pass.md) and
[world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md). The Lighting balance styles (Subtle, Soft,
Natural) keep `brilhoNasRuas` at 100%.

## Compatibility and interactions

- [world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md): the chunk maps and smoothed maps that roads read.
- [terrain-relight.md](terrain-relight.md): lot lamps must be in the terrain bake to reach roads.
- [snow.md](snow.md): the winter surface analysis shared with roads.
- No game code is patched; roads are handled entirely at the D3D9 draw level.

## Limitations

- A road chunk whose world terrain draw never ran (for example a chunk drawn only by the multi-pass terrain) has no
  registered map. That road keeps the game's lighting.
- Summer road pixel shaders receive no world height, so any future per-pixel lamp term on roads needs a vertex shader
  patch.
- The sidewalk snow blend works by the brightness of the road texture, not by a mask from the game; it applies only to
  the variants whose shader contains the blend.

## Technical reference

### Recognition (`ShaderPatches::IsRoadVs`, VS class 4)

A `vs_3_0` that:

- declares TEXCOORD1 as an output with write mask `.xy`, or `.xyzw` for the alpha-blended sidewalk variant;
- contains `dp4` with `c8` and with `c10` (world matrix rows);
- contains exactly one `mad oT1.xy, rA.xzzw, cM, cM.zwzw` (terrain UV). `M` is stored per vertex shader
  (`VsInfo::roadMap`, used as `g_curRoadMap`): `c16` in winter, `c14` in summer.

The `.xyzw` variant must also contain exactly one `mul oT1.zw, cU.xyxw, vT.xyxy` that packs the opacity UV,
where `cU` is a `def` with `x = 1.0`, `y = 2.0` and `vT` is declared as a TEXCOORD input. Arbitrary full-mask outputs are
refused; lot `.xyz` outputs are excluded.

### Known variants

| Season | VS | PS | Own map | Notes |
|---|---|---|---|---|
| Winter | VS_27AEBB08 (MD5 38A329C7, 892 bytes) = VS_2779BF40 | PS_27A1AD10 (F6ABF31E/1240) | `texld r3, v1, s6` | Road and sidewalk; sidewalk snow blend present |
| Winter | same | PS_27AE6C20 (620938B3/1320) | `s4` | Lane markings |
| Winter | same | PS_2779A7D0 (99E81A85/1288) | `texld r3, v1, s6`; last sampler s8, extra s9; already uses r6 | Sidewalk corner; blend present (`lrp r6.xyz, r2.w, r2(s8), r3(s7)`; snow `lrp r1.xyz, v2.w, r0, r3`) |
| Winter | same | PS_277978F0 (554AA649/1272, 897 prims) | `texld r3, v1, s4`; last sampler s8, extra s9; temps to r5 | Road edge blended into the terrain |
| Summer | VS_2CEAD588 (road, no tangent), C0E919AE/560 | 00E2AC7D/616, 0F907C16/568 | `texld rX, v1, s2` | Map mapping in VS `c14` |
| Summer | VS_2CEAE460 (sidewalk, with tangent), CD462949/604 | 0ABA504A/800 | `s2` | |
| (any) | Alpha-blended sidewalk (TEXCOORD1 `.xyzw`) | | | Opacity UV packed in TEXCOORD1.zw |

`PS_xxxxxxxx` probe names are object pointers and change between sessions; the 8-digit hex/size pairs are MD5-prefix
identifiers from the ground report. The winter road VS reads the snow level from `c15.z`: cover = `sat(2 c15.z)`,
height = `sat(2 c15.z - 1)`.

### Pixel patch (`ShaderPatches::PatchRoad`, `ps_3_0`)

1. Find `texld rX, v1, sL` whose result is next read, within 8 instructions, by the lamp scale `mul rY.xyz, rX, cN.x`
   (winter: immediately after, `rY = rX`, `c4.x`; summer: a few instructions later, `rY != rX`, `c3.x`). Give up if `rX`
   is read by anything else first or `rX.xyz` is overwritten (writing `rX.w` is allowed).
2. `E` = highest sampler + 1 (refused at 15), `T0` = highest temp + 1.
3. Insert `dcl_2d sE` after the last sampler declaration and, right after the `texld`:
   ```
   texld T0, v1, sE
   max   rX.xyz, rX, T0
   ```
4. **Sidewalk snow blend**, only where the shader has `mul r1.w, rA.x, rA.y` (brightness of road texture `rA`) followed
   later by `lrp r1.xyz, v2.w, ...` (the snow mix), and `maxConst + 3 < 224`: `cS` = highest const + 1 (amount),
   `cL = cS + 1` = `def (0.3, 0.59, 0.11, 0)`, `cK = cS + 2` = `def (4, -1, 0, 0)`. Before the albedo `mul`,
   `mov T1, rA` saves the road texture. After the `lrp`:
   ```
   dp3     T1.w, T1, cL           // luma of the road texture
   mad_sat T1.w, T1.w, cK.x, cK.y // sat(luma*4 - 1): only the bright parts (concrete, markings)
   mul     T1.w, T1.w, cS.x       // x Sidewalk visibility
   lrp     T2.xyz, T1.w, T1, r1   // mix the plain road texture back in
   mov     r1.xyz, T2
   ```
   The blend exists in winter variants 1 and 3 (texture in `s7`/`v0`).
5. `scaleConst` = `LampScaleAfter(...)`: the lamp-scale constant `K` when the next reader of the map value is
   `mul`/`mad rY.xyz, rX, cK.x` and `cK` is read nowhere else, never through relative addressing and not from a `def`;
   otherwise -1 (the road keeps the game's brightness).

Patched PS sizes seen in captures: 130C52EF/1284 and E7A8295D/1364.

### Draw (`DrawRoad`)

1. `PatchedFor(g_roadPs, "Road", PatchRoad)`: one patched copy per game pixel shader.
2. VS `c[M]` must equal (1/256, 1/256, 0.5, 0.5); otherwise the game draws.
3. Chunk key from VS `c8.w` / `c10.w` (world translation = chunk centre). The chunk must be registered by a world terrain
   draw (`g_chunks`), otherwise `g_lotMissing++` and the game draws. Roads do not use the world atlas: road meshes are
   per chunk, so the chunk key is exact.
4. `SamplerBind` the chunk texture (`ChunkTexture`: smoothed when ready) to `sE` (CLAMP, LINEAR, mip LINEAR).
5. If `LightmapSmooth::Find` returns the smoothed map, it also replaces the road's own copy in `sL`.
6. Sidewalk constant `cS.x` = `g_sidewalkClear` when the shader has the blend.
7. `ConstGain` multiplies `cK.x` (`scaleConst`) by `RoadGain() = 1 + (ground x road - 1) x night`.
8. Swap the pixel shader, draw, restore every touched state. Counter: "roads: N" in the developer status line.

### Registers

| Register | Content |
|---|---|
| VS `c8.w`, `c10.w` | Chunk centre (world matrix translation) |
| VS `c14` (summer) / `c16` (winter) | (1/256, 1/256, 0.5, 0.5): terrain UV mapping |
| VS `c15.z` (winter) | Snow level |
| PS `c4.x` (winter) / `c3.x` (summer) | Lamp scale of the road map |
| PS `sL` (`s6`, `s4`, `s2`) | Road's own map copy |
| PS `sE` (new) | Chunk terrain map (smoothed when ready) |
| PS `cS`, `cS+1`, `cS+2` (new) | Sidewalk amount, luma weights, (4, -1) |

Road world matrices were observed unrotated: `c8/c9/c10` = (1,0,0,896), (0,1,0,500), (0,0,1,1150) in three captures (m01,
m43, calcada-verao).

### Files

| File | Symbols | Role |
|---|---|---|
| `features/shader_patches.cpp` | `IsRoadVs`, `PatchRoad`, `LampScaleAfter`, `RoadPatch {lightSampler, extraSampler, sidewalkConst, scaleConst}` | Bytecode |
| `features/lot_light_bridge.cpp` | `DrawRoad`, `g_roadPs`, `g_curRoadMap`, `SetSidewalkClear`, `SamplerBind`, `ChunkTexture`, `RoadGain` | Draw |
| `features/lightmap_smooth.cpp` | `Find` | Smoothed chunk map |

## Rejected approaches

- Matching only the two known winter pixel shaders by bytes: corner and edge variants stayed dark. Details in
  [history](../../history/night-lighting-roads.md).
- A fixed lamp-scale position right after the `texld`: missed the summer variants.
- Marking the road partition per chunk on terrain rebuilds: belonged to the removed Smooth Streaming feature.

## See also

- [Validation](../../validation/night-lighting-roads.md)
- [History](../../history/night-lighting-roads.md)
- [Snow](snow.md), [World atlas and smoothed maps](world-atlas-and-smoothed-maps.md)
- [Engine: terrain and light bake](../../engine/terrain-and-light-bake.md)

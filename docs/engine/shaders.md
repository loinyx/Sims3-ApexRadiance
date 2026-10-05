# The game's shaders and how Apex identifies them (TS3W.exe)

This page documents the game's shader set as Apex Radiance sees it: the `Shaders_Win32.precomp` file, the shader parameters the engine registers, register conventions, lamp terms per surface, and the machinery Apex uses to recognise a game shader by bytecode. Every Night Lighting sub-part and the Light Probe depend on it.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000) unless stated |
| Used by | [Night Lighting](../features/night-lighting/README.md) and its sub-parts, [Light Probe](../features/dev-tools/light-probe.md), [Architecture: shader precompile](../architecture.md#44-shader-precompile) |
| Evidence | F7 Light Probe captures (disassembled with `D3DDisassemble`), the census of 25/09, offline scans of `Shaders_Win32.precomp`, combined-build `shader_ids.h`, `shader_patches.cpp`, `lot_light_bridge.cpp`, `wall_lamp_table.h`, `floor_atlas_table.h` |

## Overview

Engine reference for TS3W.exe 1.67.2 Steam (image base 0x00400000). Sources: F7 Light Probe captures
(`Documents\...\S3SS\LightProbe-*`, disassembled with `D3DDisassemble`), the census of 25/09, the offline scans of
`Shaders_Win32.precomp`, and the combined build's `shader_ids.h`, `shader_patches.cpp`, `lot_light_bridge.cpp`,
`wall_lamp_table.h`, `floor_atlas_table.h` (tag `combined-final`). Shader names like `PS_2669DA40` are the capture's
**pointer** at that session, not a stable id; the stable ids are size + hash ([How Apex identifies a game shader](#how-apex-identifies-a-game-shader)). *(unverified)* = not
confirmed.

Every Night Lighting sub-part works by recognising one of the game's shaders and then either changing a constant around
the draw, drawing with a patched copy, or adding a pass. This page collects what is known about the game's shader set,
its register conventions and lamp terms, and the machinery Apex uses to recognise shaders, so a new surface can be
handled without re-deriving it. The per-surface fixes are in the feature docs
([../features/night-lighting/README.md](../features/night-lighting/README.md) and its sub-pages).

## Details

### `Shaders_Win32.precomp`

- Path: `C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp` (read-only reference; never modify it).
- It holds **every** shader of the game, grouped by **technique name** (ExteriorWall, InteriorFloor, Counters...), plus
  parameter names such as `InteriorBuildingAmbientColor` and `LightingTweaks`. It is the reference for coverage tests:
  a patcher is run over every pixel / vertex shader of a technique and every result is disassembled.
- The container format was parsed by Perl scripts in the session scratchpad (`passo3\shdlist.pl`, `ground\techof.pl`,
  `ground\techps.pl`, `shd\knm.pl` / `shd\knm.tsv` = names by hash). The layout below was re-read with those scripts
  on 28/09 (file size 92,614,595 bytes); field meanings marked *(unverified)* were not confirmed.

| Part | What is known |
|---|---|
| Header | `"SPKG"`, dword 2 (version); `"SHDB"` + size 36,040,859 (the shader blob database); `"LINK"` + 15,689 (= blob count) |
| Shader blobs | 15,689 = 7,671 `"VSHD"` + 8,018 `"PSHD"`. Each: 4-byte tag, dword size, 0..4 header bytes *(meaning unverified)*, then the D3D9 bytecode, whose version token (`0xFFFE....` VS / `0xFFFF....` PS) is found at byte 8..12 of the blob; trim at the end token `0x0000FFFF`. Blob index = order in the file (0-based). `shdlist.pl` walks all of them with 0 failures |
| Techniques | From about offset 36.2 MB: `"TECH"` + name hash + pass count; per pass `"PASS"` + 2 dwords *(unverified)* + VS index + PS index (**1-based** blob indices, 0 = none), then `"PARM"` blocks (id, count, count × 12-byte entries: name hash + 4 × u16, *register mapping unverified*), then render states (count + pairs), one dword, texture bindings (index, name hash) |
| Names | `"KNM "` table near the end (offset 92,578,576): size, 1, count, then records `{hash, 0, length, chars}`; 1,281 names extracted to `knm.tsv` |
| Name hash | **FNV-1** 32-bit (multiply by `0x01000193`, then xor the byte) over the **lowercase** name, basis `0x811C9DC5`. Checked: `LightingTweaks` = `039b656e`, `ExteriorWall` = `0ddcaefb`, `StaticTerrainLightmap` = `86a5dfa4`. Note this is FNV-1 on names; Apex's shader ids ([How Apex identifies a game shader](#how-apex-identifies-a-game-shader)) use FNV-1a over bytecode DWORDs |

  Offline tools (read-only): `techof.pl <precomp> <knm.tsv> <bin>...` maps captured `.bin` files to technique names by
  MD5 (e.g. `scratchpad\censo_tech.txt`); `techps.pl <precomp> <knm.tsv> <outdir> <regex>` dumps the distinct PS/VS of
  technique families (source of `review\wallfam\`, `floorfam\`, `counter_sh\dis\`).
- Counts found (offline scans, 25/09 - 28/09):

| Technique | Pixel shaders | Vertex shaders | Notes |
|---|---|---|---|
| ExteriorWall | 58 (27 ps_3_0) | 38 | every ps_3_0 one has the `s2 x cK.x` lamp term; `K` = c2 in 26 variants, c3 in 32 |
| InteriorWall | (not counted) | | shares the per-floor wall atlas with ExteriorWall; outdoor lamps must never reach it |
| ExteriorWallAOSI, UnlitExteriorWall | | | same VS family as ExteriorWall; none matches the wall table |
| ExteriorFloors | 571 (235 ps_2_0) | 96 vs_3_0 | 261 ps_3_0 accepted by `PatchBakedAtlasPs` (`floor_atlas_table.h`) |
| InteriorFloor | 58 | | none matches the floor table |
| Counters (SM3) | 184 | 236 | exterior use: object rig; interior: lot LightMap + LightBasisMap0..3 |
| Phong (SM3) | 444 | 352 | the main object family |
| Counters + Phong, all models | 2139 variants analysed | | `scratchpad counter_sh\REPORT.md` |
| Room-lit families (InteriorWall*, InteriorFloor, FloorTileCeiling, FloorWith*, Hideable, TerrainLight_Indoor*) | 196 match "rule A" | | from the removed Native HDR analysis |

Other techniques met in captures and the census: TerrainLight (lot terrain light pass), TerrainHigh (paint layers, no
light), TerrainLow (distant terrain, light map in s3), TerrainFog, Pick* (selection), shadow and impostor passes,
GlassFor* (glass), **Night** (the HD mode: 6 per-pixel lamps, position in TEXCOORD2, lamps in c0..cN, falloff
`saturate(w/d^2)`; needs no fix), OutdoorProp, SingleObject, InstancedObject, Rug, FloorThickness, Ceiling.

### Register conventions (from captures)

Vertex shaders:

| Register | Content | Seen in |
|---|---|---|
| `c8`, `c9`, `c10` | object / chunk world matrix rows; **`c8.w` = world x, `c10.w` = world z** of the object or chunk (Apex's key for a chunk or roof) | nearly all world geometry |
| `c11` | camera position in world space (e.g. (927, 70.9, 1200)) | walls, roofs |
| `c40..c43` | camera view-projection (world -> clip), the same in every scene draw | all scene draws ([camera-and-map-view.md](camera-and-map-view.md)) |
| `c0`, `c4`, `c180`, `c192`, `c216` | other blocks that also carry the camera projection | per family |
| `c14` / `c15` / `c16` | terrain light-map uv mapping and chunk centre: lot light pass `c14` (uv) + `c15.xz` (centre); snowy lot pass `c15` + `c16`; roads `c14` (summer) / `c16` (winter); the world chunk VS has `c15 = (1/256, 1/256, 0.5, 0.5)` | terrain family |
| `c4..c7` / `c8..c11` | rig **vertex lights**: 4 directions / 4 colours, summed into COLOR0 (skinned variants: `c184`, `c188`) | objects, instanced structures |
| `c27..c29[a0.z]` / `c54..c56[a0.z]` | per-instance lamp directions / colours of instanced foliage and shrubs (vs_2_0); sun `c137/c138` or `c124/c125`; N.L clamp `max r0, r0, c123.w` (or a `def` 0 in `c117` / `c131`) | foliage |
| `c12..c21`, `c192..c201` | Phong world triples (non-skinned `c12/c15/c16/c19`, skinned `c192/c195/c196/c199`) | objects |

Pixel shaders:

| Register | Content | Seen in |
|---|---|---|
| `c0` | sun / moon colour (terrain, lot pass, roofs) | many |
| `c1..c3` / `c5..c7` | object rig: 3 lamp directions / 3 lamp colours | Phong / Counters exterior |
| `c8` / `c9` (or `c0`, `c13`) | object sun colour / direction (`ExteriorLightData`) | objects |
| `c3.x` | lamp scale of the lot light pass, outdoor walls (some variants), outdoor floors | lot / walls / floors |
| `c2.x` or `c3.x` | lamp scale of ExteriorWall (per variant; in 24 variants `c3.x` is the **bloom threshold**, not the lamp scale) | walls |
| `c4.x` | lamp scale of the snowy lot pass and winter roads | winter |
| `c7.x` | scale of the chunk light map in the world terrain shader | terrain |
| `LightingTweaks` = (0.65, 1, 4.2, 0.25) | interior floors' tone curve ([Lamp terms by surface](#lamp-terms-by-surface)) | InteriorFloor & co |

Samplers:

| Sampler | Content |
|---|---|
| `s0`, `s1` | sky cubes (ambient, reflection) |
| `s5` | sun / moon shadow map (4096^2 at the tested settings; 4 taps, 16 in Apex's roof copy) |
| `s8` | chunk terrain light map (256^2 DXT5, 4 mips) in the world terrain shader; `s7` in the 3-layer variant `PS_294418E0`; `s11` / `s12` in winter; `s3` in TerrainLow |
| `s1` | lot light map (room 0 `LightMap`) in the lot light pass |
| `s2` | room / wall light map (per-floor atlas 256x128 .. 1024x512, A8R8G8B8, managed) in walls and floors |
| `s6` | the road's own copy of the chunk light map (road variant 1; `s4` in variant 2); the scene copy for water refraction (2048x1024) |
| `s7` | wall normal map (ExteriorWall) |

Output alpha of walls, objects and roofs is a **bloom mask**: `saturate(luminance - cK.x)`, so more lamp light widens the
bloom.

### Lamp terms by surface

| Surface | Shader (capture) | Lamp term | Apex handling |
|---|---|---|---|
| World grass | terrain chunk PS (`PS_28CD3DB0`; winter `PS_295B4ED0`) | `tex2D(s8, (pos - centre)/256 + 0.5) * c7.x` | smoothed map in place of s8 ([../features/night-lighting/world-atlas-and-smoothed-maps.md](../features/night-lighting/world-atlas-and-smoothed-maps.md)) |
| Lot grass | lot terrain light pass, 568 bytes, modulate2x (DESTCOLOR / SRCCOLOR) | `tex2D(s1 lotMap) * c3.x` | replaced by a copy that takes `max(lot map, terrain map)` ([../features/night-lighting/lot-light-pass.md](../features/night-lighting/lot-light-pass.md)) |
| Lot grass, snow | `PS_2A13D200`, 1852 bytes, 12 samplers | `texld r0, v3, s2` x normal factor x `c4.x` x `def c9.x = 0.25` | bytecode patch: `max` with the terrain map x4 ([../features/night-lighting/snow.md](../features/night-lighting/snow.md)) |
| Beach lot variant | `PS_29C97D28` | `s2.rgb * c3.x^2` (already reads the terrain map) | none needed |
| Roads, sidewalks | 4 winter + 3 summer PS, road VS (`VS_27AEBB08` family) | own copy of the chunk map (`s6` / `s4`) x `c3.x` (summer) / `c4.x` (winter) | `max` with the terrain map ([../features/night-lighting/roads.md](../features/night-lighting/roads.md)) |
| Outdoor walls | ExteriorWall (`PS_2669DA40`, `PS_2734BB80` ...) | `texld r1, v1, s2` (uv = TEXCOORD1 x 1/4096 into the per-floor atlas) then `mad r5.xyz, r1, cK.x, r2` | `cK.x` x strength around the draw ([../features/night-lighting/walls.md](../features/night-lighting/walls.md)) |
| Outdoor floors | ExteriorFloors (`PS_29F3EA68` 1044 bytes; `PS_281C2318`) | `texld r0, v2, s2` (256^2 covering 64 m) then `mad r1.xyz, r0, c3.x, r1` | `max` with the world atlas ([../features/night-lighting/floors.md](../features/night-lighting/floors.md)) |
| Winter floors | `PS_1B3938E8` | lot map x factor x 0.25, no terrain map | `max` with the atlas |
| Interior walls / floors | InteriorWall / InteriorFloor | `rgb + alpha x InteriorBuildingAmbientColor` from the room map; interior floors then apply the tanh curve | none (outdoor lamps must not leak in) |
| Roofs | `PS_278ED708` 1136 bytes / VS 1192 bytes; snowy roof PS 4992 bytes | **no lamp term** (sun + sky + mask) | replacement PS / extra additive pass ([../features/night-lighting/roofs.md](../features/night-lighting/roofs.md)) |
| Lake / pond | PS 1344 bytes / VS 1088 bytes | **none** (waves, sky cubes, depth ramp, shadow, refraction from s6) | extra additive pass ([../features/night-lighting/water.md](../features/night-lighting/water.md)) |
| Ocean | `PS_1AC2FBF0` | none; real-time planar reflection (s6, 1024^2 RT) | - |
| Pool water | `PS_3140B910` (ps_2_0) | none | - |
| Objects outdoors (doors, windows, counters, props) | Phong / Counters exterior | sun + 3 rig lamps (`c1..c3` / `c5..c7`) + sky + COLOR0 vertex lights | per-pixel lamps + ground atlas ([../features/night-lighting/objects-and-rigs.md](../features/night-lighting/objects-and-rigs.md)) |
| Counters indoors | Counters interior | `max(LightMap x sat(2 N.y) x CLC.x, sum of 4 basis maps x sat(N.b) x CLC.y, sky)` | none |
| Fences, railings, stairs | instanced vs_3_0 (POSITION1 / POSITION2), e.g. `VS_216C8910`, `VS_21793C00`, `VS_21849CA0` | only the rig's overflow vertex lights (`c4..c11`), usually zero | ground atlas per pixel ([../features/night-lighting/fences.md](../features/night-lighting/fences.md)) |
| Shrubs, trees | instanced vs_2_0 + ps_2_0 (`VS_2BC7F188` / `PS_2BC82F40`...) | sun + 3 lamps per instance in the VS; the PS multiplies lamps by the **moon shadow** | shadow lerp + wrap lighting ([../features/night-lighting/foliage.md](../features/night-lighting/foliage.md)) |
| Snow on objects / stair tops | `VS_2F27C9C0` / `PS_2F27C510`; `VS_2E036438` (its PS = the snowy roof PS) | none | atlas added ([../features/night-lighting/snow.md](../features/night-lighting/snow.md)) |

Two facts that hold for every baked path: the stored lamp light is **already clipped at 1.0** (DXT5 chunk maps, A8R8G8B8
room maps), so a gain must be applied in the shader after the fetch; and the game's lamp-only scale constant (`cK.x`) is
usually read by exactly one instruction, which is what makes "scale `cK.x` around the draw" safe
(`ShaderPatches::LightMapScaleConst` checks this).

#### The interior floors' tone curve (`LightingTweaks`)

InteriorFloor, FloorTileCeiling, FloorWith* and Hideable (104 pixel shaders) apply, with `L = dot(rgb, 1/3)`:
`rgb x tanh(k L / 2) / L`, `k = LightingTweaks.z = 4.2`, as the sequence
`mul rE, -rL, cT.z; exp rE; add rE, rE, 1; rcp rE; rcp rLi, rL; mad rE, 2, rE, -1; mul rS, rLi, rE`.
The curve never reaches 1, so lamp light cannot pass white on those floors. It mattered only to the removed Native HDR
("rule T": keep the curve below 0.8 and continue along its tangent); see [../removed-features.md](../removed-features.md).

**Caution (base of `exp`):** the D3D9 `exp` instruction computes 2^x, not e^x. Read literally, the sequence gives
`2/(1 + 2^(-kL)) - 1 = tanh(k·ln2·L/2)`, not `tanh(kL/2)`. The combined build's `hdr_native.cpp` (Native HDR, removed from
the standalone) described it as `tanh(kL/2)` and computed its rule-T tangent with the natural logarithm, so its slope may
have been off by a factor ln 2 *(unverified at runtime)*. Anyone re-deriving this curve should start from the
instruction sequence, not from that comment. `.x`, `.y`, `.w` of `LightingTweaks` are of unknown meaning.

### How Apex identifies a game shader

Three levels, from strict to general (combined build files; the standalone keeps the modules):

1. **Exact bytecode** (`shader_ids.h`): size in bytes + FNV-1a 32 over the DWORDs (`h = (h ^ dword) * 16777619`, start
   `2166136261`). Only the ids are kept, never the game's bytecode (`*_ref.h` files holding bytecode must never be
   committed). Current ids:

   | Id | Size | Hash | Shader |
   |---|---|---|---|
   | `kLotLightPs` | 568 | `0xFDAD274B` | lot terrain light pass |
   | `kObjectRigPs` | 600 | `0x0A2D0BE4` | instanced outdoor objects (fences, shrubs) |
   | `kRoofPs` | 1136 | `0x6EC87E3B` | roofs |
   | `kLakePs` | 1344 | `0x4F52846A` | lake water |
   | `kSnowLotPs` | 1852 | `0x08DF01E8` | snowy lot light pass |
   | `kRoofSnowPs` | 4992 | `0x3CEB025E` | snowy roofs (also stair-top snow) |
   | `kRoofVs` | 1192 | `0x1F851ECB` | roofs |
   | `kLakeVs` | 1088 | `0x23CCB61B` | lake |
   | `kSnowLotVs` | 1400 | `0x9256F0DF` | snowy lot pass |
   | `kFloorVs` | 1492 | `0x2BC34FA8` | snowy floor tiles |

2. **Tables generated offline from the precomp**, same size + FNV-1a key: `wall_lamp_table.h` (58 ExteriorWall PS with
   their `K`), `floor_atlas_table.h` (261 ExteriorFloors PS). Generators: `scratchpad snowcover\test11.cpp` and
   `test13.cpp`.
3. **Patterns** (`shader_patches.cpp`, pure functions on the D3D9 token stream, testable offline): the parser walks
   instruction tokens (length in bits 24..27, comments skipped by their length), finds instruction and register shapes
   (`texld rX, v1, sN` followed by `mul rY.xyz, rX, cK.x`, `dp4` triples with `c8`/`c10`, `mad oT1.xy, rA.xzzw, cM,
   cM.zwzw`...), and inserts instructions using the **first free** temp register, sampler and constant. Every patcher
   fails (shader left alone) when the pattern is not exactly there. Recognisers: `IsRoadVs`, `IsFloorVs`,
   `IsSnowFloorVs`, `IsInstancedStructureVs`, `IsSnowCoverVs`, `IsSnowReliefVs`, `PatchFoliageVs`, `PatchObjectLampVs`;
   patchers: `PatchRoad`, `PatchFloor`, `PatchSnowFloor`, `PatchBakedAtlasPs`, `PatchLeafShadow`, `PatchInstancedLamps`,
   `PatchSnowCover`, `PatchSnowRelief`, `PatchObjectLampPs`. Objects use **TEXCOORD8** (no game shader among the 2139
   Counters / Phong variants uses TEXCOORD8+); floors use the first free TEXCOORD from 7.

When it happens (`lot_light_bridge.cpp`):
- **At creation**: registry `CreatePixelShader` / `CreateVertexShader` hooks at `Priority::Last` classify the bytecode
  and pre-create the patched copies (`PrecreatePs` / `PrecreateVs`), so DXVK translates them during the load and not in
  the frame where the surface first appears. The module's own creations pass through (`t_ownCreate`).
- **At first bind**: `SetPixelShader` / `SetVertexShader` hooks classify shaders created before the module was on
  (`GetFunction` to read the bytecode), and `g_stateUnknown` makes the first draw read the bound shaders from the device.
- The bytecode the game passes to `Create*Shader` has no length: `ShaderPatches::CodeBytes` walks tokens up to the end
  token `0x0000FFFF` (max 64 KB), under SEH.
- Results are cached by shader **pointer**, and every classified shader is **pinned** (AddRef until shutdown): a
  released shader's address can be reused by a new shader, which would otherwise inherit the old class.

Vertex shader classes (`g_vsCache`): 0 other, 1 roof, 2 lake, 3 snowy lot, 4 road, 5 floor (winter floor VS and
variants), 6 foliage, 7 instanced structure (fence / railing / stairs), 8 snow on objects, 9 snow with relief (stair
tops), 10 object lit by a rig, 11 snow lying on floor tiles (tested last). Pixel shader classes (`PsClass`): Other,
WorldCandidate (declares `s6` or higher: terrain), LotLight, ObjectRig, Roof, Lake, LotLightSnow, RoofSnow, WallGain,
FloorAtlas. A draw is dispatched by the pair; details in
[../features/night-lighting/README.md](../features/night-lighting/README.md).

#### Vertex-shader class tests, in order (`ClassifyVsCode`)

| Order | Class | Test |
|---|---|---|
| 1 | 1 roof | exact `kRoofVs` |
| 2 | 2 lake | exact `kLakeVs` |
| 3 | 3 snowy lot | exact `kSnowLotVs` |
| 4 | 5 floor | exact `kFloorVs` |
| 5 | 4 road | `IsRoadVs`: vs_3_0; TEXCOORD1 declared with mask .xy (terrain = full, lot = .xyz → refused); `dp4` with c8 and c10; exactly one `mad oT1.xy, rA.xzzw, cM, cM.zwzw` → map constant M (c16 winter, c14 summer) |
| 6 | 7 instanced structure | `IsInstancedStructureVs`: vs_3_0, POSITION1 + POSITION2, `mov oT1.zw, rW.xyxz`, `mad oC0.xyz, rX.w, c11, rY`, no relative addressing; optional world y + normal in TEXCOORD2 (`instWorldY`) |
| 7 | 8 snow on objects | `IsSnowCoverVs`: morph flags `slt rX, cK, v.x` (TEXCOORD1.x), `mov oT3.xy, rW.xzzw` from dp4 c8/c10 |
| 8 | 9 snow with relief | `IsSnowReliefVs`: `mul oT4.zw, rW.xyxz, cD.x` (def 0.5) and a TEXCOORD2 input |
| 9 | 5 floor variants | `IsFloorVs`: `mov o1.zw, rW.xyxz`, rW from dp4 c8/c10 (curved pool edge, m66) |
| 10 | 6 foliage | `PatchFoliageVs` succeeds (`max r0, r0, cK.w` with runtime cK, or a def of 0); the patched copy is kept |
| 11 | 10 object | `PatchObjectLampVs` succeeds (non-instanced, morphs allowed, one world triple, COLOR0 output); records worldK and the vertex-light colour base |
| 12 | 11 snow on floor | `IsSnowFloorVs` (TEXCOORD7.xy or TEXCOORD0.zw = world xz × 0.5; refuses VS with `mad oN.xy, rX, cK, cK.zwzw`) |
| - | 0 other | none matched |

Pixel-shader classes (`ClassifyPsCode`): exact ids first, then the wall table (`WallGain`, K cached per shader), the
floor table (`FloorAtlas`), then `WorldCandidate` = any PS declaring a sampler ≥ s6 (dcl `0x0200001F`, register type
10). A WorldCandidate is a terrain chunk only if `RecordWorldChunk` passes at draw time (VS c15 = (1/256, 1/256, 0.5,
0.5) and a 2D 256×256 texture with ≤ 5 levels, not Q8W8V8U8, in s15..s1).

#### Draw dispatch (`OnDrawInner`, first match wins)

1. ObjectRig PS → `DrawObjectRig`; 2. Roof PS → `DrawRoof`; 3. RoofSnow PS, unless the VS is class 9 → `DrawRoofSnow`;
4. Lake PS → `DrawLake`; 5. foliage VS → `DrawLeafShadow`; 6. WallGain PS → `DrawWallGain`; 7. if the lot light bridge
is off: stop here (combined build: HDR gain only); 8. road VS → `DrawRoad`; 9. floor VS → `DrawFloor`; 10. FloorAtlas
PS (not with the snow-floor VS) → `DrawFloorAtlas`; 11. instanced VS → `DrawInstanced`; 12. snow-cover VS; 13.
snow-relief VS; 14. object VS → `DrawObjectLamp`, **falling through** when the object patch does not apply (class 10
also matches some roof and snow VS); 15. LotLightSnow PS → `DrawLotSnow`; 16. WorldCandidate PS → `RecordWorldChunk` +
smoothed-map swap, or `DrawSnowFloor` when it is not a chunk; 17. LotLight PS → the replacement lot pass; otherwise the
snow-floor VS draws not claimed by any PS class.

#### Pattern matchers (`shader_patches.cpp`)

| Function | Finds | Inserts |
|---|---|---|
| `PatchRoad` | `texld rX, v1, sL` scaled by `mul rY.xyz, rX, cN.x` within 8 instructions (rX.xyz untouched between); sidewalk: `mul r1.w, rA.x, rA.y` + `lrp r1.xyz, v2.w, r0, r3` | `texld rT, v1, sExtra; max rX.xyz, rX, rT` (sampler = max + 1, temp = max + 1); sidewalk blend `sat(luma·4 − 1)·cS.x` |
| `PatchFloor` | winter floor `texld rA, v2, s2` … `mul rB.xyz, rC.w, rA` | widens `dcl v0` to .zw (world xz); atlas texld + `max` |
| `PatchSnowFloor` | `texld rL, vK, sM` then `mul rB.xyz, rS.s, rL` (writes to .w in between ignored) | `max(rB, atlas)` at TEXCOORD7.xy or TEXCOORD0.zw |
| `PatchBakedAtlasPs` | the single 2D `texld rL, vK, sM` whose next rgb read is `mad rX.xyz, rL, cK.x, rY`, K read once, before any branch | `max rL, rL, atlas(TEXCOORDn.xy)` |
| `PatchLeafShadow` | `lrp rD.w, tN.x, cK.y, rS.w` (moon shadow; a def cK must hold 1) | `rD.w = lerp(rD.w, 1, cN.x)` as add + mad (one constant per instruction: valid ps_2_0) |
| `PatchFoliageVs` | `max r0, r0, cK.w` | sun keeps the clamp; lamps `r0.yzw = max(N·L/1.5 + 1/3, 0)` via `def c255 = (1/1.5, 0.5/1.5)` |
| `PatchInstancedLamps` | the single `add rD.xyz, rS, vCOLOR0` | `rT = max(atlas(vT.zw·cA.xy + cA.zw)·cB.x, vC)`, optional per-pixel lamps |
| `PatchSnowCover` | `mad rL.xyz, rX.w, c0, rY` … `mul oC0.xyz, rL, rA`, no flow control | `rL.xyz += atlas·cB.x` before the mul |
| `PatchSnowRelief` | sky `texld rC, rN, s0` then `mad rL.xyz, rC, cK.x, rS`, outside the `rep` loops | `rL.xyz += atlas·cB.x` (caller doubles the uv scale) |
| `PatchObjectLampVs` | one world triple `dp4 rW.x/y/z` with consecutive cK..cK+2 (open-triple tracking; with several, the one from POSITION, else the root) | `dcl_texcoord8 oN.xyz` + `mov oN.xyz, rW.xzy` (floors: first free TEXCOORD from 7) |
| `PatchObjectLampPs` | shapes A (sky mad + chain ending `mad rD.xyz, rS.w, c7, rD`, optional `add vC`), B (chain without cube), C (no lamps; `max rX, vC, rY`) | 8 per-pixel lamps (`colour·min(1, W·sat(N·l)/d²)`, W = 0.4 × range) and ground `atlas·(0.5 + 0.5 N.y)·cB.x`, combined by `max` with the game's rig + vertex lights |

#### Offline coverage results

| Check | Result | Where |
|---|---|---|
| Roads | 118 unique captured shaders: 3 road VS, 7 road PS (4 winter + 3 summer), all valid; sidewalk only in variants 1 and 3 | NOTAS "Ruas SEM neve" |
| Foliage VS / leaf-shadow PS | VS 6 → 8 of 77, PS 2 → 3 of 99 (`test7.cpp`), later +7 PS with def-1 components; road VS correctly refused | NOTAS "Arbustos de inverno", "Plantas de verao" |
| Instanced structures | 3 of 116 captured VS | NOTAS "CERCAS: causa real" |
| Snow cover / relief | only VS_2F27C9C0 / PS_2F27C510; only VS_2E036438 of 161 VS | NOTAS 25/09 ~10:00-10:40 |
| Snow floor VS | only m29 / m69 / m71 after the review (5 terrain/water VS matched before) | NOTAS "Revisao profunda" |
| Objects | after v5.6: VS 588/588; Counters PS 136/184 (A 128, B 8); Phong PS 426/444 (A 394, C 32); 0 invalid. Not covered: 34 PS using all of v0..v9, 32 Counters group D | `scratchpad\counter_sh\` (perl simulators reproduce the C++ counts) |
| Wall table | 58 ExteriorWall PS, K = c2 in 26 / c3 in 32; 0 of 27 other-family PS | `snowcover\test11.cpp` |
| Floor table | 261 accepted of 571 ExteriorFloors (235 ps_2_0); 0 of 58 InteriorFloor | `snowcover\test13.cpp` |
| In-game census (dev) | 84 VS/PS pairs with baked light or an outdoor rig, 49 unfixed at 25/09 17:05 | `censo_tech.txt` |

#### Constant ranges of Apex's own replacement shaders

Must stay clear of game registers and of each other.

| Shader | Registers |
|---|---|
| Summer lot pass `kReplacementHlsl` | c0..c4 (game values), samplers s0 / s1 / s2 (terrain map or atlas) / s5, TEXCOORD1/2/4/5 |
| `kObjectRigHlsl` (instanced shrubs/fences) | c0, c1, c3.x = night level |
| Roof `roof_ps.hlsl` | c20..c35 lamp position + radius, c36..c51 colour, c52 params (x strength, y count) |
| Snowy roof `roof_snow_lamps_ps.hlsl` | same block c20..c52, c53.x = VS c15.x |
| Water `water_lamps_ps.hlsl` | c20..c59 (lamps, c52 params, WVP rows c53..c56, translation c57, depth linearisation c59), INTZ in s7 |
| Object / fence patches | `atlasConst`, `strengthConst`, `lampParamConst` (0, strength, 0, 1e-4) + 2 × 8 lamp blocks, all taken from the first free constant of each shader; objects use TEXCOORD8 |
| Planned per-pixel walls/floors (PASSO3-PLANO.md 3.4) | c64..c149, **never built** |

### Validation and limits

- Every patch is tested offline over all captured shaders and, where possible, over the whole technique in the precomp,
  and every output is disassembled with `D3DDisassemble` (harnesses in the scratchpad; see
  [../features/dev-tools/census.md](../features/dev-tools/census.md) and [../workflow.md](../workflow.md)). Test
  executables must not write many files (Kaspersky flagged one as ransomware).
- ps_3_0 allows 512 instruction slots on native D3D9 (DXVK reports 32768; the combined build logs the device limits at
  startup), and at most 10 input registers. ps_2_0 has tighter rules: `lrp` with two constant sources is invalid on
  native D3D9 even though DXVK accepts it (bug found and fixed on 25/09).
- In developer mode, Apex saves every refused shader (`ShadersRecusados\`, named by content hash, at most 300 per session) and the
  census lists every outdoor VS/PS pair that no fix took, with its refusal reason.

### Which Apex features depend on what

| Item | Apex users |
|---|---|
| Exact ids ([How Apex identifies a game shader](#how-apex-identifies-a-game-shader), item 1) | Night Lighting: lot pass, snowy lot pass, roofs, lake, snowy roofs, instanced objects' moon shadow |
| Wall / floor tables | Night Lighting: walls, outdoor floors |
| Pattern recognisers | Night Lighting: roads, floors, snow, fences, foliage, objects |
| `c8.w` / `c10.w` keys, `c15` terrain mapping | Night Lighting: chunk map recording (`RecordWorldChunk`), roofs, lake lamp choice |
| `c40..c43` and the projection shape | PostScene camera vote (dev profiler fallback); Reflections uses the water VS WVP instead |

### Pitfalls

- Capture names (`PS_xxxxxxxx`) are pointers of one session: identify shaders by size + hash or by pattern.
- Do not assume a constant is the lamp scale because it multiplies a map: in 24 ExteriorWall variants `c3.x` is the
  bloom threshold. Check that `cK` is read once and not defined by a `def`.
- The same pixel shader can serve two surfaces (snowy roofs and stair-top snow share one PS): dispatch by the VS first.
- The TEXCOORD0.zw "world xz" shape of snowy floor VS is also used by terrain and lot passes: that class is tested last
  and only handles draws no pixel-shader class took.
- The water pass disables Z and unbinds depth: mark such passes `DepthShare::SetInternalPass` or the post-scene trigger
  fires mid-frame ([../architecture.md](../architecture.md), post-scene chain).

### Open items

- The precomp container format (only known through the scratchpad scripts).
- Exact counts per technique for InteriorWall, and the full technique list.
- Why some variants keep the lamp scale in `c2` and others in `c3` (per-material build options, *unverified*).

## Address reference

Parameters are registered by name through `0x0079A160(name, a, b)` into handles kept in globals; the binder of a draw
writes pointers into the shader parameter table `*0x011D7530`, and the effect pass uploads them right before the draw,
in the same call tree, on the render thread ([light-objects-and-rigs.md](light-objects-and-rigs.md), rig binder).

| Registered at | Names | Meaning |
|---|---|---|
| `0x006A4700` (lot lighting) | `LightMap`, `SpilloverLightMap`, `WorldToLotTransform`, `LightmapSizeParameters`, `LotSizeParameters`, `LightBasisMap0..3`, `CounterLightingConfig` | room / lot light maps and their mapping ([room-light-maps.md](room-light-maps.md)); `CounterLightingConfig` = (2, 0, 0, 5) or (2, 0.85, 1, 1) chosen by `lotMgr+0x288` |
| rig binder `0x006B8B30` | `LightDirections` / `LightColors` (rig `+0x10` / `+0x50`, 4 each: sun + 3 lamps), `VertexLightDirections` / `VertexLightColors` (rig `+0x90` / `+0xD0`), `HDLight*` (rig `+0x08`), `TreeLightColors` / `TreeLightDirections` (SpeedTree path) | per-object light rig ([light-objects-and-rigs.md](light-objects-and-rigs.md)) |
| precomp parameter names | `InteriorBuildingAmbientColor` (c2 in InteriorWall, c4 in InteriorFloor), `LightingTweaks` (c1 or c2), `ExteriorLightData` (sun, c8/c9 in objects) | captures + precomp |
| `0x00C292B0` terrain bake | `staticTerrainLightmap` technique; params `Lighting/RenderLightmap/NormalMap`, `Lighting/RenderLightmap/HeightMap` | [terrain-and-light-bake.md](terrain-and-light-bake.md) |

## See also

- [Light objects and rigs](light-objects-and-rigs.md): the rig binder.
- [Room light maps](room-light-maps.md).
- [Terrain and light bake](terrain-and-light-bake.md).
- [Camera and map view](camera-and-map-view.md): projection constants.
- [Night Lighting](../features/night-lighting/README.md).

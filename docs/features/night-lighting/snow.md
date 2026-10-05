# Snow

In winter, lamp light on snow matches the light on the ground around it. Snowy lots no longer show a hard cut where
street-lamp light stops at the lot border, and snow on floor tiles, around pools, on door sills, on stair tops and on
fence tops is lit like the ground next to it. Snowy roads, roofs, foliage, ponds and the lamp colour on snow have their
own pages (see [See also](#see-also)). Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0. Subdued daytime response on snow on objects and *Ground brightness* on the squared terrain variant: in development (PR #2) |
| Default | On (with Night Lighting and *Street lamps light lots*) |
| Menu | No snow-specific switch. Uses Lighting > Ground (*Street lamps light lots*, *Smooth ground light*, *Ground brightness*) and Lighting > Objects > *Doors, counters and fences* |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawLotSnow`, `DrawSnowFloor`, `DrawSnowOnObject`), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchSnowFloor`, `PatchSnowCover`, `PatchSnowRelief`) |

## The problem

In winter the game swaps many surfaces to snow shader variants, and each one misses lamp light in its own way:

- **Snowy lot ground.** The lot light pass multiplies the lot light map by 0.25 (`def c9.x`), so the lot term is four
  times weaker than the world terrain term, and the lot map does not contain street lamps. Lot borders show a strong cut
  in snow.
- **Snow on floors and door sills** reads only the lot's room light map, so street lamps never reach it.
- **Snow on stair tops and fence tops** is lit by the moon and sky only, with no lamp term at all.

The formulas measured in captures:

| Surface | Shader | Lamp term |
|---|---|---|
| World snow terrain | PS_28FDBAE0 | albedo x (terrain map x `c7.x` (= 1) + 0.65 x ambient cube + specular) |
| Lot snow (light pass, modulate2x over the base pass PS_255BBEC8) | PS_2A13D200 (1852 bytes) | albedo x (lot map x direction factor x 0.25 + 0.65 x ambient + specular). Simple lots do not bind the direction maps `s7..s10`, so the factor is about N.y = 1 |
| Snow road | PS_27A1AD10 | Own copy of the chunk map in `s6`, without the lamps ([roads.md](roads.md)) |

## How Apex Radiance solves it

Every snow fix reads the world light atlas (the smoothed ground light of every chunk, see
[world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)) at the pixel's world position and takes the
brighter of it and the game's term, or adds it where the game has no lamp term.

1. **Snowy lot ground:** the lot light pass takes `max(lot term, terrain x 4)`; the x4 cancels the pass's 0.25 so lot and
   world match.
2. **Snow on floors and door sills:** `max(room map term, atlas)` before the game's lamp scale.
3. **Snow on stair tops:** atlas x strength added to the moon and sky light, outside the shader's noise loops. These
   draws share their pixel shader with snowy roofs and are told apart by the vertex shader.
4. **Snow on fence tops and props:** atlas x strength added to the moon and sky light before the snow texture multiply.

Fixes 3 and 4 follow the fence switch and strength, including the subdued daytime response of
[fences.md](fences.md). Fixes 1 and 2 use the game's own lamp scale, multiplied by *Ground brightness*.

## Settings

Snow has no setting of its own besides sidewalk snow. It uses these Night Lighting settings:

| Menu label | TOML key | Type | Default | Range | Effect on snow |
|---|---|---|---|---|---|
| Street lamps light lots | `luzDoPosteNaGramaDoLote` | bool | on | | Dispatch gate for every snow fix ([lot-light-pass.md](lot-light-pass.md)) |
| Smooth ground light | `mapaDeLuzSuavizado` | bool | on | | Builds the world atlas that every snow fix reads |
| Ground brightness | `brilhoNoChao` | float | 100% | 25 to 300% | Scales the lot snow pass (`c4.x`) and snow on floors, weighted by the night level |
| Fences and stairs catch light | `cercasComLuzDoChao` | bool | on | | Snow on fence tops (class 8) and stair tops (class 9) |
| Fence brightness | `forcaNasCercas` | float | 100% | 25 to 200% | Strength of the atlas term on class 8 and 9 snow |
| Sidewalk visibility | `calcadaComNevePisada` | float | 50% | 0 to 100% | How much sidewalk shows where Sims have walked ([roads.md](roads.md)) |

## Compatibility and interactions

- [floors.md](floors.md): winter floor tiles and the curved pool edge (VS class 5).
- [fences.md](fences.md): the fence switch, strength and daytime response also drive class 8 and 9 snow.
- [roofs.md](roofs.md): the snowy roof pass shares its pixel shader with stair snow.
- [roads.md](roads.md): snowy roads and sidewalk snow.
- [foliage.md](foliage.md): winter bushes and trees. [water.md](water.md): ponds in snow.
  [lamp-colour.md](lamp-colour.md): the stock pink lamps that show on white snow.
- **Ground brightness on terrain variants:** *Ground brightness* also reaches a world terrain shader whose lamp map is
  multiplied by `c4.x x c4.x` through a scalar temporary, next to the linear `c7.x` path. Without this, snow and grass
  drawn by the two variants showed a boundary whenever the gain was not 100%. The classifier decides by bytecode, never
  by snow or lot names; shared constants and unknown layouts keep the game's brightness. See *Technical reference*.
- No game code is patched for snow.

## Limitations

- A winter lot light pass drawn with VS 436BB272/1348 (seen once, a town hall lot) is not handled. `DrawLotSnow` requires
  the exact `kSnowLotVs`, and that VS keeps the chunk centre in `c15` and the UV mapping in `c14` (the known one uses
  `c16`/`c15`).
- The atlas is ground light without height: snow on a high fence top gets the ground's value.
- The `IsSnowFloorVs` shape (TEXCOORD0.zw = world xz / 2) also appears in 12 terrain and lot winter pairs. Three guards
  keep it off them (class 11 tested last, used only when no pixel shader class claimed the draw, room-map texture check).
- The lot snow pass and snow on floors have no snow-specific strength: the atlas enters at the game's lamp scale.

## Technical reference

### Surfaces and shaders

| Surface | Capture | Shaders | Game's lamp light | Fix |
|---|---|---|---|---|
| Snowy lot ground | LightProbe-neve, m04 | VS_2A13C7D8 (`kSnowLotVs`, 1400, `0x9256F0DF`) / PS_2A13D200 (`kSnowLotPs`, 1852, `0x08DF01E8`) | `texld r0, v3, s2` x normal factor x `c4.x` x 0.25 | `PatchSnowBytecode`, `DrawLotSnow` |
| Snowy floor tiles | m08 | `kFloorVs` / PS_1B3938E8 | Lot map x factor x 0.25 | `PatchFloor` ([floors.md](floors.md)) |
| Curved pool edge (winter floor + pool mask) | m66 | VS_29991640 / PS_29F52F90 | Same | `IsFloorVs` + `PatchFloor` |
| Snow mesh on lot floor (around the pool) | m69 | VS_2FA6DE10 / PS_2FA6D640 (2748 prims) | `texld r0, v1, s6`, `mul r1.xyz, r1.z, r0`, x `c3.x`; the VS writes TEXCOORD7.xy = world xz x 0.5, which the PS never reads | Class 11 (tc 7), `PatchSnowFloor` |
| Door sills with snow | m71, m72 (also m29) | VS_27CE3AD0 (= VS_215055E8 of m29) / PS_2A044CD0 | `texld r0, v1, s1` (32x64 in m71, 512x128 in m72), `mul r0.xyz, r1.w, r0`; VS TEXCOORD0.zw = world xz x 0.5 (`mul o1.zw, r2.xyxz, c20.x`) | Class 11 (tc 0), `PatchSnowFloor` |
| Snow on stair tops | m50, m51 | VS_2E036438 / PS_2E036820 (`ps_3_0`, 4992 bytes, 304 slots; 4 `rep i0` loops of 3D noise, `s1` permutation 256x256, `s2` gradient 256x1) | None: `dp3_sat r1.w, c1, N` x 4-tap shadow (`s5`) x `c0` + cube `s0` x `c5.x` (`mad_pp r0.xyz, r0, c5.x, r1`), x `r6.w`, x `r6` albedo, fog `v4` | Class 9, `PatchSnowRelief` |
| Snow on fence tops, rails, props | m48 | VS_2F27C9C0 / PS_2F27C510 (#126/#139, 2138 tris) | None: `r1 = N.L_moon x shadow x c0 + cube(0,1,0) x c4.x`, then `mul oC0.xyz, r1, snowTex(s1, uv = world x 0.125)` | Class 8, `PatchSnowCover` |

### Snowy lot ground (`PatchSnowBytecode` + `DrawLotSnow`)

- Gate: PS class `LotLightSnow` (exact 1852 bytes) and VS class 3 (exact `kSnowLotVs`), VS `c15.xy` = 1/256.
- `PatchSnowBytecode`, made once from the bound shader:
  - requires the `dcl` of `s11` and the first `texld r0, v3, s2`, immediately followed by `mul r0.xyz, r1.w, r0`
    (light-basis factor);
  - inserts after the `dcl` of `s11`: `dcl_texcoord1_pp v7.xy` (tokens `0200001F 80010005 90230007`) and `dcl_2d s12`
    (`0200001F 90000000 A00F080C`);
  - inserts after the `mul`:
    ```
    texld_pp r7, v7, s12          // 03000042 802F0007 90E40007 A0E4080C
    add_pp   r7.xyz, r7, r7       // x2
    add_pp   r7.xyz, r7, r7       // x4: cancels the later "mul r0.xyz, r4, c9.x" (0.25)
    max_pp   r0.xyz, r0, r7
    ```
  - patched output identity: C61F8B55/1940.
- `DrawLotSnow`: the terrain source is the world atlas when ready. The VS computes the UV as
  `(world.xz - c16.xz) * c15.xy + c15.zw`, so `c15` becomes `(a.x, a.y, a.z + c16.x a.x, a.w + c16.z a.y)` for the draw.
  Without the atlas: the home chunk map at key (`c16.x`, `c16.z`) (smoothed if ready), else `g_lotMissing++` and the game
  draws. Binds `s12` (CLAMP, LINEAR, mip LINEAR, sRGB off; only differing states are set and restored), swaps the pixel
  shader, multiplies PS `c4.x` by `GroundGain()` (`ConstGain`), draws, restores `s12`, `c15` and the shader. Counter
  "snow: N".

### Snow on floors and door sills (VS class 11, `PatchSnowFloor` + `DrawSnowFloor`)

- `IsSnowFloorVs(t, &tc)`: a `vs_3_0` with exactly one `mul oTn, rW.swz, cH.s` where either (tc 7) TEXCOORD7 is declared
  `.xy` and written by `mul oT7.xy, rW.xzzw, cH.s`, or (tc 0) TEXCOORD0 is declared full and written by
  `mul oT0.zw, rW.xyxz, cH.s`; `cH.s` is a replicated component of a shader `def` equal to exactly 0.5; `rW.x` / `rW.z`
  come from `dp4` with `c8` / `c10`. It refuses any VS containing `mad oN.xy, rX, cK, cK.zwzw` (the terrain-map UV of
  terrain, lot and water vertex shaders, which share the TEXCOORD0.zw shape).
- Class 11 is tested last in `ClassifyVs` (after the object patch). `DrawSnowFloor` runs only when no pixel shader class
  claimed the draw: from the world-candidate branch when `RecordWorldChunk` fails (these pixel shaders declare `s6` and
  up), or from the "not LotLight" fallback. Runtime guard: the map sampler found by the patch must hold an A8R8G8B8,
  non-DEFAULT pool texture of at most 1024x1024 (a room map); terrain maps are DXT5.
- `PatchSnowFloor(t, tc, FloorPatch&)` (`ps_3_0`, no flow control): the single `texld rL, vK, sM` whose next reader is
  `mul rB.xyz, rS.s, rL` with a replicated swizzle on `rS` (writes to `.w` between them are ignored: m69 has `max r0.w`).
  tc 7: new input `dcl_texcoord7 vV.xy` (refused if TEXCOORD7 is already read); tc 0: the TEXCOORD0 declaration is
  widened to `.xyzw`. After the `mul`:
  ```
  mad   T.xy, vV.(xyxy | zwzw), cA, cA.zwzw
  texld T, T, sE
  max   rB.xyz, rB, T
  ```
  `mapSampler` = M, used by the runtime guard; `scaleConst` = the lamp scale after the `max` (`LampScaleAfter`).
- `DrawSnowFloor`: atlas mapping with `c.xy x 2` (the coordinate is world xz / 2); separate caches per tc
  (`g_snowFloorPs` for 7, `g_snowFloorPs0` for 0); `ConstGain` multiplies `scaleConst` by `GroundGain()`. Counter "snow
  on floors: N".

### Snow on stair tops (VS class 9, `PatchSnowRelief`)

- The stair snow pixel shader is byte-identical to the snowy roof one: PS_2E036820 = PS_2793C3E8 = PS_27D4DAE0 =
  `kRoofSnowPs` (4992 bytes, `0x3CEB025E`). `OnDrawInner` sends `RoofSnow` to `DrawRoofSnow` only when the VS is not
  class 9.
- `IsSnowReliefVs`: TEXCOORD4 output with `.zw`, a TEXCOORD2 input (the snow's base position; roofs of the same family have
  none), exactly one `mul oT4.zw, rW.xyxz, cD.x` with `def cD.x = 0.5`, `rW.x` / `rW.z` from `dp4` with `c8` / `c10`.
- `PatchSnowRelief` (`ps_3_0`, requires `dcl_cube s0` and TEXCOORD4 with `.zw`): after the single
  `mad rL.xyz, rCube, cK.x, rS` outside any `rep`/`loop` (`rCube` = the last `texld rCube, rN, s0`), insert
  ```
  mad   T.xy, vT4.zwzw, cA, cA.zwzw
  texld T, T, sE
  mad   rL.xyz, T, cB.x, rL          // + atlas x strength, before the "x r6.w" and "x r6" (albedo)
  ```
- `DrawSnowRelief` = `DrawSnowOnObject(..., posScale = 2)`: atlas `c.xy x 2`, `cB.x = SurfaceLampGain(night,
  forcaNasCercas)`. Needs `cercasComLuzDoChao`. Counter "snow with relief: N".

### Snow on fence tops and props (VS class 8, `PatchSnowCover`)

- `IsSnowCoverVs`: TEXCOORD3 output `.xy`, TEXCOORD1 input, the morph flags `slt rX, cK, vT1.x`, exactly one
  `mov oT3.xy, rW.xzzw` with `rW.x` / `rW.z` from `dp4` with `c8` / `c10` (the VS morphs between two positions, `v0` and
  TEXCOORD0).
- `PatchSnowCover` (`ps_3_0`, refuses any flow control): the single `mul oC0.xyz, rP, rQ`; of the two operands, the one
  whose last writer is a `texld` is the snow texture, and the other (`rL`) must be written by `mad rL.xyz, rX.w, c0, rY`
  (moon x shadow + sky). Before that `mul`:
  ```
  mad   T.xy, vT3.xyxy, cA, cA.zwzw
  texld T, T, sE
  mad   rL.xyz, T, cB.x, rL          // like the ground: light map + ambient
  ```
- `DrawSnowCover` = `DrawSnowOnObject(..., posScale = 1)`, with the same switch and strength. Counter "snow on objects:
  N".

### New resources per handler

| Handler | New sampler | New constants | Position source | Scale |
|---|---|---|---|---|
| Lot snow | `s12` (fixed) | None (VS `c15` overwritten per draw) | TEXCOORD1 (`v7`) | x4, then the game's `c4.x` x 0.25, x `GroundGain` |
| Snow floor tc 7 / tc 0 | max + 1 | `cA` | TEXCOORD7.xy / TEXCOORD0.zw = world xz / 2 | The game's lamp scale after the `max`, x `GroundGain` |
| Stair snow | max + 1 | `cA`, `cB` | TEXCOORD4.zw = world xz / 2 | `cB.x` = `SurfaceLampGain(night, fence strength)` |
| Snow cover | max + 1 | `cA`, `cB` | TEXCOORD3.xy = world xz | `cB.x` = `SurfaceLampGain(night, fence strength)` |

### Ground brightness on squared terrain variants

`ShaderPatches::LightMapScaleConst(t, sampler, &squared)` finds the constant that scales the chunk light map in a world
terrain pixel shader (`TerrainLampConst` caches it per shader and logs `[LotLightBridge] Ground brightness: terrain shader
XXXXXXXX, light map sN -> cK.x` or `no lamp-only scale found, left as the game`):

- **Linear:** `LampScaleAfter` finds `mul`/`mad rY.xyz, rL, cK.x` with `cK` read nowhere else (`c7` in the captured
  summer and winter chunks).
- **Squared:** after the map fetch, a `mul rS.<c>, cK.x, cK.x` into one component of a temporary, then
  `mul rY.xyz, rL, rS.<c>` (replicated swizzle, no saturate or modifiers, no flow control, no relative addressing,
  `cK` not a `def` and read exactly twice). Then `squared = true`.

`TerrainConst` writes `LampScale(native, night, gain, squared)` for the draw: `weighted = 1 + (gain - 1) x night`; at full
night `native x weighted` (linear) or `native x sqrt(weighted)` (squared, so the product `c4.x x c4.x` scales by
`weighted`); by day the `DayLampScale` term is added (`(1 - night) x gain`), as `native x weighted + day` or
`sqrt(native^2 x weighted + day)`. The captured multi-pass world light shader (`kWorldMultiLightVs`) always uses `c3`
squared.

### Files

| File | Symbols | Role |
|---|---|---|
| `features/lot_light_bridge.cpp` | `PatchSnowBytecode`, `DrawLotSnow`, `g_snowPs` | Snowy lot ground |
| | `DrawSnowFloor`, `g_snowFloorPs`, `g_snowFloorPs0`, `g_curSnowFloorTc` | Snow on floors and sills |
| | `DrawSnowOnObject`, `DrawSnowCover`, `DrawSnowRelief`, `g_snowCoverPs`, `g_snowReliefPs` | Snow on objects and stairs |
| | `OnDrawInner` (`RoofSnow && !g_curVsIsSnowRelief`), `TerrainLampConst`, `TerrainConst` | Stair routing, terrain brightness |
| `features/shader_patches.cpp` | `IsSnowFloorVs`, `PatchSnowFloor`, `IsSnowCoverVs`, `PatchSnowCover`, `IsSnowReliefVs`, `PatchSnowRelief`, `IsFloorVs`, `PatchFloor`, `LightMapScaleConst` | Bytecode |
| `features/terrain_lighting_policy.h` | `LampScale`, `DayLampScale`, `SurfaceLampGain` | Brightness and daytime policy |
| `shaders/shader_ids.h` | `kSnowLotPs`, `kSnowLotVs`, `kRoofSnowPs`, `kFloorVs` | Exact identities |

## Rejected approaches

- Classifying by the shared snowy roof pixel shader before the vertex shader: stair snow went to the roof pass. Details in
  [history](../../history/night-lighting-snow.md).
- Calling the snow-floor fix only from its own branch: it never ran, because those shaders fall into the world-candidate
  branch.
- Accepting any VS with the TEXCOORD0.zw shape for snow floors: it caught terrain and water.
- One patched cache for both snow-floor variants: tc 0 and tc 7 need separate copies.
- Reusing `PatchSnowCover` for stair snow: it refuses flow control and stair snow has four `rep` loops.

## See also

- [Validation](../../validation/night-lighting-snow.md)
- [History](../../history/night-lighting-snow.md)
- [Roads](roads.md), [Roofs](roofs.md), [Fences](fences.md), [Floors](floors.md), [Foliage](foliage.md),
  [Water](water.md), [Lamp colour](lamp-colour.md)
- [World atlas and smoothed maps](world-atlas-and-smoothed-maps.md)

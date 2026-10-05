# Lot light pass (street lamps inside lots)

Street-lamp light continues from the world grass onto lot grass instead of stopping in a straight line at the lot
border. Near the border the two sides blend over 3 m, so a lamp standing next to a lot edge leaves no step. Part of
[Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (part of Night Lighting since 0.1.0). Lot UV alignment, daylight terrain term and the constant-read guard: in development (PR #2) |
| Default | On |
| Menu | Lighting > Ground > Ground & Lots > *Street lamps light lots* |
| Configuration | `[patches.NightTerrainRelight]` `luzDoPosteNaGramaDoLote` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`kReplacementHlsl`, lot branch of `OnDrawInnerCore`), [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) (experimental game patches) |

## The problem

World grass and lot grass are lit by two different formulas (Light Probe captures `LightProbe-lote` and
`LightProbe-mundo`):

- **World grass** is drawn by the terrain chunk shader, which adds `tex2D(s8 terrainLightMap, uv) * c7.x` to sun and
  sky. The map is 256x256 per 256 m chunk: the pre-baked "stamp" of the lamps, wide soft circles.
  uv = `(position - chunk centre) / 256 + 0.5`; the chunk centre is in VS c8.w / c10.w.
- **Lot grass** is drawn by several lot terrain passes. Its light pass is a 568-byte pixel shader, blended modulate2x
  (DESTCOLOR / SRCCOLOR), that adds `tex2D(s1 lotLightMap) * c3.x`. The lot light map is solved on the CPU by
  `FUN_006be020`: colour x light+0x130 x intensity x N.L / d^2, with d measured from the lamp head. Street lamps arrive
  faint.

The two formulas meet at the lot border: a straight cut. The lot pass vertex shader already outputs the terrain-map uv in
TEXCOORD1 (mapping in VS c14, chunk centre in c15.xz), so the fix only needs the pixel shader. See
[engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md).

## How Apex Radiance solves it

The lot light pass is redrawn with a replacement pixel shader that takes `max(lot light map, terrain light)`, where the
terrain light is the [world light atlas](world-atlas-and-smoothed-maps.md) (or, before the atlas exists, the light map
of the lot's home chunk). The terrain map contains lot lamps too when [terrain relight](terrain-relight.md) bakes them.

Per draw, when the bound PS is `PsClass::LotLight` (exact `kLotLightPs`, 568 bytes, FNV-1a `0xFDAD274B`):

1. The replacement `kReplacementHlsl` (ps_3_0) is created from the start-up bytecode (`EnsureReplacement`, `CompilePs`);
   status "Active" or "Failed: <error>".
2. VS c14..c15 are read; c14.xy must be 1/256 (tolerance 1e-5), otherwise the game draws.
3. Terrain source: the world atlas when ready (c14 rewritten for this draw), else the home chunk's map.
4. Soft lot edges: the lot rectangle is matched and PS c28..c30 built.
5. Lot map uv alignment and gains go into PS c31.
6. The original PS c28..c31 are read; if that read fails, the game draws unchanged.
7. Terrain texture bound to s2, replacement drawn, everything restored.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Street lamps light lots | `luzDoPosteNaGramaDoLote` | bool | on | | This pass; off also disables roads, floors, fences, snow and per-pixel objects (everything after the wall handler in the dispatch) |
| Smooth ground light | `mapaDeLuzSuavizado` | bool | on | | Needed for the atlas; without it the pass uses the home chunk's game map |
| Ground brightness | `brilhoNoChao` | float | 100% | 25 to 300% | Night-weighted scale of c3.x |
| Lot lamp brightness | `forcaDasLampadasDoLote` | float | 100% | 25 to 300% | Night-weighted scale of the lot's own map (c31.x) |
| Developer: Soft lot edges (A/B) | `bordaSuaveLote` | bool | on | | Registered only in developer mode (always on otherwise); pushed every frame (`SetSoftLotEdges`) |
| Individual options | `gramaDoLoteUsaLuzDoLote` | bool | off | | Experimental; see *Experimental game patches* |
| Individual options | `qualidadeAltaEmTodosOsLotes` | bool | off | | Experimental |
| Individual options | `postesAcesosNoCalculo` | bool | off | | Experimental |

The full table is in the [Night Lighting overview](README.md#settings).

## Compatibility and interactions

- [World atlas and smoothed maps](world-atlas-and-smoothed-maps.md): source of the terrain term. A chunk map that
  changes is never read stale: `Find` and the atlas return the game's current map or a plain copy until the smoothed
  map is ready.
- [Terrain relight](terrain-relight.md): the terrain map must contain the lamps (dusk rebuild, lot lamps in the bake)
  for the max to help. The verified lot pass also reports which lots are visible, which drives the arrival relight.
- Roads, floors, snow, fences and objects all require *Street lamps light lots* (dispatch order).
- The snowy lot pass is a different shader, bytecode-patched rather than replaced: [snow.md](snow.md) "Snowy lot ground".

## Limitations

- The terrain stamp has no wall occlusion (inferred): near lamps, the max can show terrain light inside fenced or
  walled lot areas.
- The winter lot pass with VS 436BB272 (m58, "prefeitura") is not handled ([snow.md](snow.md)).
- The snowy lot pass has no soft edge.
- A porch lamp within 3 m of the edge fades towards the atlas near the edge (with *Lot lamps light the street* the atlas
  contains it, so the change is small).
- A lot the tree walk does not list (no lighting manager, or room 0 not rebuilt: +0xC0 == 0) draws the plain max
  (counted "without a lot rectangle").
- Lots on chunks drawn only by unrecognised world terrain variants never get a chunk registered; without the atlas they
  count as "without terrain texture".
- Lot UV alignment applies only to the exact recognised lot VS; other layouts keep native coordinates.

## Technical reference

### The replacement shader (`kReplacementHlsl`, ps_3_0)

| Register | Content |
|---|---|
| c0 | sun colour (game) |
| c1.xyz | sun direction (game) |
| c2 | shadow-map offsets (game) |
| c3.x | lamp scale (game; times the ground brightness for this draw) |
| c4.x | sky scale (game) |
| c28 | soft edges: `(dLx/du, dLx/dv, Lx0, W)`: lot-local x in metres = `dot(float3(terrainUv, 1), c28.xyz)`; W = lot width |
| c29 | soft edges: `(dLz/du, dLz/dv, Lz0, D)`: lot-local z; D = lot depth |
| c30 | soft edges: `(1 / band, bias, 0, 0)`: `(1/3, 0)` on, `(0, 1)` off (w = 1 everywhere) |
| c31 | `(lot map gain, daylight terrain term, lot uv scale, lot uv offset)`: x = `LotMapGain()`, y = `DayLampScale(night, ground gain)`, z/w = `1/63` and `-c12.x x 16/63` for the recognised VS, else 0 |
| s0 | sky cube (game) |
| s1 | lot light map (game) |
| s2 | terrain light (atlas or chunk map) |
| s5 | sun/moon shadow map (game) |
| TEXCOORD1.xy | terrain uv |
| TEXCOORD2 | shadow projection |
| TEXCOORD4 | normal |
| TEXCOORD5 | lot map uv |

Body, equal to the game's pass except the lamp lines:

```
sun     = lerp(avg of 4 tex2Dproj shadow taps, 1, edge fade) * saturate(dot(n, c1.xyz))
terrain = tex2D(sTerrain, terrainUv.xy).rgb * (c3.x + c31.y)
lp      = (dot(float3(terrainUv.xy, 1), c28.xyz), dot(float3(terrainUv.xy, 1), c29.xyz))   // lot-local metres
e       = min(lp, (c28.w, c29.w) - lp);  w = smoothstep01(saturate(min(e.x, e.y) * c30.x + c30.y))
lotUv   = lotUv * (1 + c31.z) + c31.w
lamps   = lerp(terrain, max(tex2D(sLot, lotUv).rgb * c31.x * c3.x, terrain), w)   // game: tex2D(sLot).rgb * c3.x
col     = sun * c0.rgb + lamps
col     = texCUBE(sSky, n).rgb * c4.x + col
return float4(col * 0.5, 0)                                                          // modulate2x
```

At night c31.y = 0 and the result equals the earlier night shader. By day the terrain term gains
`(1 - night) x ground gain`, matching the world terrain's daylight term ([world-atlas](world-atlas-and-smoothed-maps.md));
the native lot/window map scale is unchanged. fxc check: `fxc /T ps_3_0 /E main` on the string's contents.

### Terrain source

- **World atlas** when `LightmapSmooth::Atlas(a)` is ready. The summer lot VS computes the map uv as
  `(world.xz - c15.xz) * c14.xy + c14.zw`; for the atlas (`uv = world.xz * a.xy + a.zw`) c14 becomes
  `(a.x, a.y, a.z + c15.x * a.x, a.w + c15.z * a.y)` for this draw. c15 also feeds another uv (c13), so only c14 changes.
  The atlas coordinates are zero-initialised, also when smoothing is off or the atlas is not ready.
- **No atlas**: the chunk texture recorded from the world chunk draw with key `Key(c15.x, c15.z)` (the home chunk), or
  its smoothed version (`ChunkTexture` -> `LightmapSmooth::Find`). With no recorded chunk, `g_lotMissing++` ("without
  terrain texture") and the game draws.
- s2 is set to CLAMP/CLAMP, LINEAR min/mag/mip, sRGB off through `SamplerBind`; texture, sampler states, c14, PS
  c28..c31 and the PS are restored after the draw. Counter `g_lotDrawn` ("lot light fixed: N draws").

### Soft lot edges

Within `kEdgeBand` = 3 m of the lot rectangle, the lamp term blends from `max(lot, atlas)` to the atlas term:
`w = smoothstep(0, 3, distance to the nearest edge)`, `lamps = lerp(atlas, max(lot, atlas), w)`. At the edge w = 0, so
the lot grass shows exactly the terrain term the world grass shows on the other side; the smoothstep has zero slope at
the edge, so the lot side continues the terrain's own gradient; 3 m inside and beyond nothing changes. It can only
lower the lamp term (never below the atlas), so lot edges without a lamp are unchanged. Two lots that share an edge both
fade to the same atlas at it. Only this pass (lot ground) is touched.

- **Lot-local position.** The PS receives the terrain uv `(world.xz - c15.xz) * k.xy + k.zw` (k = the c14 the draw runs
  with). The lot pass VS has the lot matrix in c8 / c10 (`world.x = c8.x lx + c8.z lz + c8.w`,
  `world.z = c10.x lx + c10.z lz + c10.w`). `LotEdgeConstants` inverts both on the CPU (double precision) into
  `lotLocal = A * uv + b`, so the shader does two dot products and works for any lot rotation.
- **Lot size**: room 0 of the lot, +0xC0 / +0xC4 (tiles along lot-local x / z; 1 tile = 1 m). Decompile evidence:
  `FUN_006a2740` (room rebuild) sets room 0's +0xC0 / +0xC4 from the manager's tile grid size +0x264 / +0x268 (and
  +0x20 / +0x28 = size - 1, +0x1C / +0x24 = 0: the tile bounds); `FUN_0069efc0` walks tiles `[0, C0) x [0, C4)`; the
  manager's room-id grid +0x260 is bounds-checked with +0x264 / +0x268 in `FUN_006a4300`, `006a4890`, `006a4a00`;
  `FUN_006a4c10` publishes "LotSizeParameters" = (+0x264 / 64, +0x268 / 64); `FUN_006c6ab0` gathers world lights at the
  lot centre `(C0 / 2, 0, C4 / 2)` through +0xF8.
- **Matching the draw to its lot**: `RefreshLotRects` (Present, every 20 frames, or 5 frames after a lot pass found no
  rectangle) walks the light update tree like LightDiag (tracker + 0x6A0 + level x 0x1A4, levels `kLotLevels` 0..7 then
  -1..-4, manager room hash +0x234 / +0x238, room 0) and keeps `{origin m12/m14, m0, m8, W, D, lot id}` from room 0's
  +0xF8 matrix and +0xC0 / +0xC4. `FindLotRect` matches VS c8.w / c10.w within 5 cm and c8.x / c8.z within 2e-3.
  Room 0's +0xF8 matrix equals the lot pass VS c8 / c10 (verified on lot 09080020A1D28860).
- The matched lot id is also recorded as visible (`VisibleLot`: seen within 500 ms; first sight or reappearance after
  2 s counts as an arrival), even with soft edges off. [Terrain relight](terrain-relight.md) uses it.

### Lot map uv alignment (`kLotLightVs`)

The recognised regular lot VS (680 bytes, FNV-1a `0x0C8CC5E8`, `VsInfo::contractedLotUv`) maps lot-local xz to the lot
map as `(xz x 63/64 + 1/4) x c12`. That contraction made points just outside a closed wall sample the indoor row of the
CPU-solved lot map. When VS c12.x is finite, in (0, 1] and equal to c12.y, c31.z = 1/63 and c31.w = -c12.x x 16/63, so
`lotUv x 64/63 - c12 x 16/63 = local xz x c12`. Light intensity and texture read count are unchanged; the world atlas and
the CPU light map are untouched. Other VS layouts get c31.zw = 0 (native coordinates).

### Experimental game patches (Individual options, off by default)

Each was one of the failed attempts recorded in the history and is kept as a switch.

| Key | Site | What it does |
|---|---|---|
| `postesAcesosNoCalculo` | 0x006BE18C `movaps xmm0,[esi+0E0h]` (`0F 28 86 E0 00 00 00`) in `FUN_006be020` (street-lamp class light evaluation, vfunc+0x4C, reads effective colour +0xE0) | `StreetLampColourStub`: for a light with the lit flag (+0x100 & 0x20) clear, type +0xB0 == 0xB and lot id (+0xC0 or +0xC4) == 0, returns colour +0xF0 x intensity +0x10 instead of +0xE0 (0 while off), so a lot solved by day still gets street lamps. Counter "Street lamps counted as lit". `PASSO3-PLANO` F-J6: any future lamp packer must copy this rule |
| `gramaDoLoteUsaLuzDoLote` | 0x00C7F87D `mov eax,[edi+0D8h]; test eax,eax` (`8B 87 D8 00 00 00 85 C0`); context `F3 0F 10 05 38 A5 07 01 F3 0F 11 44 24 18 74 13`; null-bind path at 0x00C7F8B7 (`A1 80 CE 1E 01 6A 00 6A 00`) | `LotPassStub` keeps the lot pass of `FUN_00c7f750` (per-chunk light/fog pass; lot pass when `[ebp+0Ch]` = 1) on "no terrain lightmap" after a full rebuild, because the rebuilt chunk+0xD8 has no street-lamp light inside lot footprints. Float at 0x0107A538 stored at `[esp+18h]`. The world pass is untouched |
| `qualidadeAltaEmTodosOsLotes` | 0x00ADB66B, 0x00ADB884 `mov byte [esp+0Ch],0` (`C6 44 24 0C 00`) in `FUN_00adb5a0` / `FUN_00adb850` | The quality flag passed to `FUN_006a5ef0` (active lot or Build mode) becomes 1: every lot solved at the active lot's quality. Lots loaded afterwards |

### Files and functions

| File | Function | Role |
|---|---|---|
| lot_light_bridge.cpp | `kReplacementHlsl`, `kReplacementPsId`, `EnsureReplacement`, `CompilePs` | Replacement shader |
| | `OnDrawInnerCore` (last block) | The lot pass redraw |
| | `LotRect`, `ReadLotRects` (SEH walk), `RefreshLotRects`, `FindLotRect`, `LotEdgeConstants`, `g_softEdges`, `kEdgeBand`; `SetSoftLotEdges`, `LotEdgeStatus` | Soft lot edges |
| | `VisibleLot`, `LotVisible`, `TakeLotArrivals` | Visible lots for the arrival relight |
| | `LotMapGain`, `GroundGain`, `ConstGain` | Gains |
| | `LotLightBridge::SetEnabled`, `Status`, `OnWorldChanged` (`ClearChunks`), `Shutdown(keepChunkMaps)`, `ChunkCount` | Lifecycle |
| shader_ids.h | `kLotLightPs` {568, 0xFDAD274B}, `kLotLightVs` {680, 0x0C8CC5E8} | Exact gates |
| lightmap_smooth.cpp | `LightmapSmooth::Atlas`, `Find`, `Get` | Terrain light source |
| night_terrain_relight_patch.cpp | `LotPassStub`, `StreetLampColourStub`, `kQualitySites` | Experimental game patches |

## Rejected approaches

- Changing the world light collection radius, re-solving room 0, rebuilding the type-5 terrain textures: no effect.
- High quality on every lot, the null terrain bind in the lot layer, street lamps counted as lit: no fix alone (kept as
  experimental switches).
- Home-chunk sampling with CLAMP: stretched the chunk's last row at a chunk border (m76); the atlas is used instead.
- Lot size from the lot map uv rect, or a coverage signal in the lot map, or vertex-buffer bounds: not reliable.

Details in [history](../../history/night-lighting-lot-light-pass.md).

## See also

- [Validation](../../validation/night-lighting-lot-light-pass.md)
- [History](../../history/night-lighting-lot-light-pass.md)
- [Terrain relight: lamp change tracking](terrain-relight.md)
- [World atlas and smoothed maps: chunk registration](world-atlas-and-smoothed-maps.md)

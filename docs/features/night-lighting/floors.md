# Floors

Outdoor floor tiles (decks, patios, the park fountain plaza, pool edges, snowy lot tiles) are lit by street lamps and
lot lamps as brightly as the grass beside them. In the unmodified game these floors use only a baked per-lot floor map,
which misses street lamps and in summer is nearly black. Floors of upper stories also receive the outdoor lamps of every
story. Interior floors are unchanged. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (present since 2.1.0, the first version in this repository) |
| Default | On (follows the Ground & Lots switches) |
| Menu | No card of its own: Lighting > Ground (*Street lamps light lots*, *Smooth ground light*, *Ground brightness*) and Lighting > Stories |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawFloor`, `DrawFloorAtlas`), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchFloor`, `PatchBakedAtlasPs`), [`shaders/floor_atlas_table.h`](../../../shaders/floor_atlas_table.h), [`shaders/shader_ids.h`](../../../shaders/shader_ids.h) |

## The problem

| Case | Capture | Shaders | Game's lamp light |
|---|---|---|---|
| Snowy lot floor tiles | m08 | VS `kFloorVs` (1492 bytes, FNV-1a `0x2BC34FA8`, MD5 `9B6DB72A`) / PS_1B3938E8 (1724, MD5 `A9849757`) | only the lot map (256x128) x direction factor x 0.25, no terrain map |
| Curved pool edge, winter | m66 | VS_29991640 (1064) / PS_29F52F90 (1620): winter floor with a pool mask (texkill, s11 at TEXCOORD7) | same family |
| Summer outdoor floor | m61 (pixel 1763,849) | VS_281C2AE8 / PS_281C2318 (1044 bytes, MD5 `1788C5B6`) | `texld r0, v2, s2` then `mad r1.xyz, r0, c3.x, r1`. Floor map 256x256 covering 64 m (uv = pos x 0.5 / 64), nearly black: mean 0.002, max 0.18, one lamp spot with steps |
| Park fountain plaza | chafariz #8 | PS_214701C0 (the same 1044-byte shader) | room light map 512x256 A8R8G8B8 in s2; the fenced plaza is a separate room with 0 lights |
| Flat floor variant | telhado #116, telhado2 #51 | 812-byte PS (MD5 `9BF0A41E`): `mov_sat r0.w, c1.y`, cube read at a constant up vector | as above |
| Test ground | m60 | VS_29F3E810 / PS_29F3EA68 (the same 1044 bytes) | s2 map alpha 0 everywhere, RGB max 0.176: room 0 with almost no light under the game's own law |
| Snow lying on floors, door sills | m69, m71/m72 | see [snow.md](snow.md) | room map only |

The 1044-byte PS (m60, m61, fountain) hashes to `0x9CFC614F` and the 812-byte PS to `0x802006F6`; both are entries of
`floor_atlas_table.h`. A balanced floor also needs the outdoor lamps of every story, which each story's own map lacks
(the floor-line cut, see [level-light-share.md](level-light-share.md)).

## How Apex Radiance solves it

Patched copies of the floor shaders take `max(floor map, world light atlas)` at each pixel, where the atlas is the
smoothed ground light that grass and lots use ([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)).
The game's lamp scale after that `max` is multiplied by *Ground brightness*, blended by the night level.

1. **Winter floors and the pool edge** (`DrawFloor`, VS class 5): the floor VS already outputs world xz; `PatchFloor`
   inserts the atlas read and the `max` after the lot-map term.
2. **Summer outdoor floors** (`DrawFloorAtlas`, PS class `FloorAtlas`): a VS copy writes world xz to a free TEXCOORD;
   `PatchBakedAtlasPs` inserts the atlas read and the `max` before the map's scale, for the 261 ExteriorFloors pixel
   shaders in `floor_atlas_table.h`.
3. **Floors of upper stories** get the outdoor lamps of every story through [level-light-share.md](level-light-share.md).

## Settings

Floors have no setting of their own. They depend on:

| Menu label | TOML key | Why |
|---|---|---|
| Street lamps light lots | `luzDoPosteNaGramaDoLote` | The floor handlers sit after the bridge-enabled check in the draw dispatch |
| Smooth ground light | `mapaDeLuzSuavizado` | The atlas exists only with it |
| Ground brightness | `brilhoNoChao` | Scales the floor's lamp constant after the `max` (`GroundGain`, `1 + (gain - 1) x night`) |
| Outdoor light between floors | `luzExternaEntreAndares` | Floors of other stories get the outdoor lamps |

These settings are documented on their own pages ([lot-light-pass.md](lot-light-pass.md),
[world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md), [level-light-share.md](level-light-share.md)).

## Compatibility and interactions

- [snow.md](snow.md): snow meshes on floors, door sills and pool edges use `DrawSnowFloor` (VS class 11), with the same
  `max(room map, atlas)`.
- [objects-and-rigs.md](objects-and-rigs.md): the summer floor VS `898DEAF3` also classifies as class 10 (object VS);
  `DrawObjectLamp` needs rig mode 1 or 2 and falls through for floors, and the dispatch checks `FloorAtlas` before the
  object branch.
- The summer floor PS (1044 bytes) declares s6+ and would be a `WorldCandidate`, but `RecordWorldChunk` fails (VS c15 is
  (-0.0005, 1.5, 5, 1)); without `FloorAtlas` these floors would keep only the faint lot map.

## Limitations

- The atlas crosses walls: a bright pool from a lamp beside a garden wall reaches the tiles behind it. The game never
  occlusion-tests floor samples either (1179 floor samples observed, none tested).
- The 235 ps_2_0 ExteriorFloors pixel shaders, and any ps_3_0 variant the pattern refuses, keep the game's look.
- Ground under an up-pointing type-4 spot 0.1 m above the tile is dark under the game's own law; it is the wrong place to
  judge floor light.
- Interior floors are not changed: InteriorFloor shaders are never in `floor_atlas_table.h`, and rig-mode checks keep the
  object path off indoors. InteriorWall/Floor, Ceiling, Rug and FloorThickness need nothing.

## Technical reference

### Winter floors (`DrawFloor`)

- VS class 5 = exact `kFloorVs` (`{1492, 0x2BC34FA8}`), or `ShaderPatches::IsFloorVs`: a vs_3_0 whose full TEXCOORD0
  output gets `mov oT0.zw, rW.xyxz` exactly once, where rW.x = `dp4 ..., c8` and rW.z = `dp4 ..., c10` (world xz).
- `PatchFloor(t, FloorPatch&)` (ps_3_0 only):
  1. find the first `texld rX, v2, s2` (lot map) and after it `mul rB.xyz, rS.w, rX` (map x bump / light-basis factor);
  2. widen `dcl_texcoord0 v0` to `.xyzw` (the shader read only `.xy`; `.zw` = world xz);
  3. add `dcl_2d sE` (E = highest sampler + 1), constant cA (highest + 1), temp T (highest + 1);
  4. after the mul insert:
     ```
     mad  T.xy, v0.zwzw, cA, cA.zwzw   // atlas uv
     texld T, T, sE
     max  rB.xyz, rB, T
     ```
  Refused if a piece is missing, samplers >= 15 or constants >= 224.
- `LampScaleAfter` finds the game's lamp scale after the `max` (m08: `mul r0.xyz, r2, c2.x`, read once) as `scaleConst`.
- `DrawFloor` binds the atlas to sE (CLAMP, LINEAR, mip NONE), sets cA = atlas mapping, swaps the PS, multiplies
  `scaleConst.x` by `GroundGain()` (`ConstGain`, skipped at 1), draws and restores.
- Net formula: `max(lotMap x dir x 0.25, atlas) x c2.x x GroundGain`. The lot term keeps the game's 0.25 and the atlas
  enters at full scale.

### Summer outdoor floors (`DrawFloorAtlas`)

- `floor_atlas_table.h`: 261 ExteriorFloors ps_3_0 pixel shaders (size and FNV-1a) that `PatchBakedAtlasPs` accepts and
  that pass `D3DDisassemble` after patching. The package has 571 ExteriorFloors PS, 235 of them ps_2_0; none of the 58
  InteriorFloor PS matches. Generated offline (scratchpad `snowcover/test13.cpp`, families from `passo3/ground/techps.pl`).
- Dispatch: `g_curClass == FloorAtlas && !g_curVsIsSnowFloor`, after the class-5 floor branch.
- Vertex shader: a copy made once per game VS with `PatchObjectLampVs(t, needColor0 = false, &tc)`, which finds the world
  position `dp4` triple (c8/c9/c10) and writes world xz to the first free TEXCOORD from 7 up (many floor VS already use
  TEXCOORD7). All 96 vs_3_0 ExteriorFloors VS export. Log: `[LotLightBridge] Outdoor floor: vertex shader ... patched` or
  `without the expected pattern (TEXCOORDn)`.
- Pixel shader: `PatchBakedAtlasPs(t, tc, FloorPatch&)`, cached per TEXCOORD index (`g_floorAtlasPs[tc]`):
  1. before the first flow-control instruction, the single `texld rL, vK, sM` (2D sampler, input coordinate) whose next
     RGB reader is `mad rX.xyz, rL, cK.x, rY`;
  2. cK read by no other instruction;
  3. refused if TEXCOORDn is already read or inputs >= 9;
  4. insert `dcl_texcoordN vV.xy`, `dcl_2d sE`, and right before that mad:
     ```
     mad  T.xy, vV.xyxy, cA, cA.zwzw
     texld T, T, sE
     max  rL.xyz, rL, T
     ```
- `DrawFloorAtlas` binds the atlas and cA, sets both copies, scales `scaleConst` by `GroundGain()`, draws, restores both.

| Patch | Registers added | Coordinates |
|---|---|---|
| `PatchFloor` | sE = max sampler + 1, cA = max const + 1, T = max temp + 1 | v0.zw (widened dcl) |
| `PatchBakedAtlasPs` | new input vV = max input + 1 declared as TEXCOORDn.xy, sE, cA, T | TEXCOORDn from the VS copy, n >= 7 |

### Engine facts

| Fact | Evidence |
|---|---|
| Floor VS `898DEAF3` (864 bytes, summer, the VS of `1788C5B6`): `r1 = (0.5 v0.x, y, 0.5 v0.y, 1)`, `mul r1.xy, c18.x, v0`, `mul o3.xy, r1, c12` -> TEXCOORD2.xy = (lx, lz) x c12; `mov o2.w, r0.y` -> world y in TEXCOORD1.w | PASSO3 F-J3, confirmed by the critique |
| `room+0xF8` of the floor's room: pointer to a 4x4 float matrix, row-vector convention; VS c8 = (m0, m4, m8, m12), c10 = (m2, m6, m10, m14) | PASSO3 F-J2 (`fn_006aabe0.c`); runtime values unverified |
| Outdoor floor maps (desc+0x44) do not go through the class-2 wall blur | PASSO3 F-J1 |
| The game runs no wall test for ground or floor samples | PASSO3 critique F9 |
| The two floor variants differ in constants: `1788C5B6` has `def c10, 100, 0.75, -0, 0` and `def c11`, normal-map scale c8.x, final `mul oC0.xyz, r0, c9.x`; `9BF0A41E` has `def c9`, `def c10`, final `mul oC0.xyz, r0, c8.x` | PASSO3 critique F3. The pattern patch appends after the highest constant; an HLSL replica must follow each |

No game code is patched for floors.

### Diagnostics (developer mode)

Developer status *Street lamps on lots*: counters `floors: N` (`DrawFloor`) and `outdoor floors (summer): N`
(`DrawFloorAtlas`). F7 on a floor shows the `mod:` line and PS size. Log kinds `Floor` and `Outdoor floor` in
`[LotLightBridge] Shaders at their first draw`; in developer mode refused shaders are saved to
`Apex Radiance\ShadersRecusados`.

## Rejected approaches

- An exact-byte class for the floor VS: missed the pool edge; replaced by the `IsFloorVs` pattern. Details in
  [history](../../history/night-lighting-floors.md).
- Per-pixel floor lamps with HLSL replicas of `1788C5B6` / `9BF0A41E` (PASSO3 increment 2): planned, replaced by the
  pattern approach.
- Writing world xz to TEXCOORD7 unconditionally: many ExteriorFloors VS already use it.

## See also

- [Validation](../../validation/night-lighting-floors.md)
- [History](../../history/night-lighting-floors.md)
- [Walls](walls.md), [Light between stories](level-light-share.md), [Snow](snow.md),
  [World atlas and smoothed maps](world-atlas-and-smoothed-maps.md)

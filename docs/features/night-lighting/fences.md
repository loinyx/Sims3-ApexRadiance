# Fences, railings, posts and stairs

At night, fence rails and posts, railings and outdoor stairs are lit by the lamps around them: faces turned toward a lamp
are brighter, faces turned away and fences behind a wall stay dark. Snow lying on fence tops and stair tops follows the
same switch and strength. By day the lamp term fades to 3% of its night strength (the shared daytime curve). Part of
[Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (present since 1.0.0). Subdued daytime lamp response: Released in 2.6.0. Per-pixel lamps, walls blocking them and the game's own lighting in full daylight: Released in 2.7.0. Lighting corrections: Released in 2.12.1. The outdoor walls' lamp law with *Realistic lamp reach*, slabs blocking lamps below and tops above the lamp heads without ground light: Released in 2.12.3. The lot's ground lamp shade on pieces near the ground (*Walls shade lamp light near the ground*), upper-story railing and parapet tops without a ground lamp's pool, lamps tested from the piece nearest them: Released in 2.13.0. The reach law's scale without the outdoor walls' gain, the lamp sum held under 1 by its brightest channel, faces reading the ground lamp shade in front of them, gates closing their fence's gap in the lot shade: Released in 2.15.3. Elevated structures follow the architectural lamp gain and keep the ground pool out: Released in 2.15.4. A larger lamp pick memo, kept with lamps shining out of a house: Released in 2.17.1. |
| Default | On |
| Menu | Lighting > Game fixes > Objects > *Fences and stairs catch light*, *Fence brightness* |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`IsInstancedStructureVs`, `PatchInstancedLamps`, `AppendGroundShade`), [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawInstanced`, `FindLotAt`, `FloorShadeBind`), [`features/ground_lamp_shadow.h`](../../../features/ground_lamp_shadow.h) (`FloorShadeRows`), [`features/terrain_lighting_policy.h`](../../../features/terrain_lighting_policy.h) (`SurfaceLampGain`) |

## The problem

The game draws fences, railings and stairs as instanced "scene model arrays", and their vertex shader takes lamp light
only from the rig's four "vertex light" slots. The game fills those slots only with overflow lights, beyond the first
four gathered, so they are almost always zero. One rig also serves a whole fence group from the group's centre, which can
fall inside a roofed room and then sees only that room's lights. The result: fences next to brightly lit ground get no
lamp light at all. See [engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## How Apex Radiance solves it

Elevated instanced runs follow the architectural lamp gain with Realistic lamp reach and exclude the ground atlas pool. The lowest instance origin selects the existing ground-height blend, preserving ground runs and stairs that extend down to the ground. A verified complete floor tile immediately below an emitter casts a soft per-pixel shadow on each selected lamp separately.

Apex Radiance patches the fence pixel shader by pattern so that valid groups use per-pixel lamp light, as on outdoor objects:
the lamps around the fence group, with angle and distance. Where the group's position cannot be read, the lamp term
falls back to the brighter of the game's vertex lights and the world ground-light atlas at the pixel's world position.

1. **Recognise the instanced structure vertex shader** by its instance streams and output shape.
2. **Patch the pixel shader** once per game shader: up to 8 per-pixel lamps (colour x sat(N.l) x sat(1 - d^2/R^2)^2,
   from the world normal and height the instanced vertex shader already passes in TEXCOORD2) replace shared vertex lamp light when the group has valid spatial data. Missing spatial data keeps
   the original vertex/atlas fallback. With *Realistic lamp reach* on, a second copy takes the outdoor walls' law and
   native lamp scale instead (F7 20-31-19: a balcony band went black at 7.82 m from a post while the wall behind it stayed lit;
   see [objects-and-rigs.md](objects-and-rigs.md#realistic-reach-on-objects-fences-and-windows)), with the walls' *Brightness* gain for elevated architectural runs. Ground-level runs retain their fence gain; the fence strength then acts as a trim over its 75% default. Only a group whose pieces were read (lamps placed) takes that copy; otherwise the
   first copy draws. F7 on a fence, railing or balcony band shows `FENCE/STAIRS (instanced) lit by the mod` with the lamps
   placed, the strengths and the `lamp law:` line.
3. **Choose the lamps per group.** The group's centre and its two ends are read from the instance stream (the mean of the
   instance positions, the farthest instance from it, then the farthest from that one; read again every 5 s), with up to
   32 of its pieces taken evenly through the buffer. Lamps within 40 m plus the group's half length (at most 60 m more) are
   chosen around the centre. A lamp is dropped when an outside wall or a placed upper floor stands between it and the piece
   nearest it and both ends (0.5 m above the pieces), so a fence wholly behind a wall loses it and one that only runs past a
   wall keeps it. (The group's centre is not the first point: for an L-shaped band it stood in open air 3.6 m
   from its nearest piece, F7 18-17-00 #393; the centre is still used when no piece was read.) With valid spatial data, the atlas contributes only to upward-facing surfaces; vertical faces use directional lamps.
   The atlas on those tops is weighted by `GroundLightGate` ([`ground_light_gate.h`](../../../features/ground_light_gate.h)):
   the height gate (none of it from 1.5 m over the highest head) counts only the heads of lamps in the atlas, not
   the indoor lamps shining out of a house; and the weight is multiplied by the share of the atlas lamps' light the group
   sees past walls and slabs (as an object's ground share). Without that gate the parapets and balustrade caps of every story
   over an arcade took the red ground pool of a post under the arcade's slab (F8 2026-10-14 20-50-03): the indoor lamps of
   story 2 at 59.1 m opened the gate, and the atlas has no slab.
   A group whose centre cannot be read gets no spatial lamps and retains the native atlas fallback.
   Each lamp keeps the game's cones: a face off a wall sconce's cones takes only its spill, as the game's own
   cone law gives it (`0x006BC940`; see the pixel shader patch below), so a brick fence face beside a sconce aimed down
   its wall is no longer lit at full strength. Shared group brightness does not set a minimum for valid spatial groups.
   **Pieces near the ground take the lot's ground lamp shade**: the lamp sum is multiplied per pixel by the shade
   the lot pass gives the grass under them ([lot-light-pass.md](lot-light-pass.md), as the story-0 outdoor floors,
   [floors.md](floors.md)), within about 1 m of the lot's ground. A curb or low fence behind a free-standing wall no longer
   keeps a lamp on the other side because another part of its group sees it; balcony railings and stair tops higher up
   keep the per-group test alone. A face reads the shade 0.6 m in front of it along its normal
   (`GroundShadow::kFenceShadeReach`); tops read it where they stand. The fence is itself a wall in the lot's mask, so at
   its own footprint a face turned to the lamp read the edge of its own shadow: a dark band under 1 m. Fence gates are
   objects ([objects-and-rigs.md](objects-and-rigs.md)); one standing in its fence's gap closes that gap in the lot shade
   ([lot-light-pass.md](lot-light-pass.md#lamp-shade-on-lot-ground-groundshadow)).
4. **Draw with the atlas bound** and the strength and lamps set per draw; restore everything afterwards.
5. **Daytime response:** the strength sent to the shader is `SurfaceLampGain(night, strength)` = strength x
   `LampDay(night)`: the configured strength at full night, 3% of it in full daylight, `0.03 + 0.97 smoothstep(0, 1,
   night)` between (2.12.3; until 2.12.2 a linear fade toward `0.08 x min(strength, 1)` and no draw at all at a night
   level of 0.01 or less). Shared vertex lamp lighting remains only in the fallback path; sunlight, geometry and materials are untouched.

There are two fence shader families in the game. The `vs_2_0` instanced "object rig" family shares its shaders with
bushes and is fixed by the foliage and rig changes ([foliage.md](foliage.md), [objects-and-rigs.md](objects-and-rigs.md)).
This page covers the `vs_3_0` instanced structure family.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Fences and stairs catch light | `cercasComLuzDoChao` | bool | on | | Ground light on fences, railings, posts and stairs, and on snow lying on objects and stair tops ([snow.md](snow.md)) |
| Fence brightness | `forcaNasCercas` | float | 75% | 25 to 200% | Multiplies the atlas term and the per-pixel lamps; 100% = the same light as the ground. With *Realistic lamp reach* on, the per-pixel lamps take the outdoor walls' native lamp scale (not the walls' *Brightness*) and this value (with *Seamless light brightness*) is a trim over its 75% default. Shown when the switch is on |

Both apply live (`LotLightBridge::SetFenceGroundLight`, every frame). The Game fixes section disables these rows unless
*Street lamps light lots* and *Smooth ground light* (Lighting > Game fixes) are both on (the atlas exists only with both) and offers
a button to turn them on.
The Lighting balance styles set `forcaNasCercas` to 67.5% (Subtle), 75% (Soft) or 100% (Natural).

## Compatibility and interactions

- Snow on fence tops (VS class 8, `PatchSnowCover`) and on stair tops (class 9, `PatchSnowRelief`) use the same switch,
  strength and daytime response ([snow.md](snow.md)).
- Fenced yards: lamps reach their rig through the CPU room-gather thunk at `0x006BBE70` in `ObjectLightBridge`
  ([objects-and-rigs.md](objects-and-rigs.md)).
- `PS_2673D758`, a fence and railing pixel shader, also matches the outdoor object patch; the instanced structure class is
  dispatched first.
- Per-pixel lamps follow *Seamless light on pieces* (`luzPorPixelNosObjetos`) and its brightness
  (`forcaLuzPorPixelNosObjetos`) in the same Game fixes section ([objects-and-rigs.md](objects-and-rigs.md)); walls, and the placed
  floors of upper stories above a lamp, block them only while Lighting > Outside > Objects > *Buildings block lamp light on trees and objects* is
  on ([level-light-share.md](level-light-share.md)).
- No game code is patched for the shader part; recognition is by instruction shape.

## Limitations

- Final release-build gameplay confirmation is pending; offline checks do not establish identical material appearance or frame-time cost.

- Release cleanup has not yet received gameplay confirmation on the exact release artifact; see validation.

- One lamp list serves a whole fence group (up to 8 lamps around its centre); a very long group can miss a lamp near one
  end. Walls and slabs block a lamp only when they hide it from the piece nearest it and both ends of the group;
  otherwise it keeps the largest share of the three (a slab's edge dims it). Within about 1 m of the lot's ground the
  ground lamp shade corrects this per pixel; higher up (balconies, stair tops) it does not.
- The ground lamp shade is one factor for all lamps together (the lot mask's alpha, the share of lamp light the outside
  walls leave on the grass): a curb lit by a second lamp that no wall hides is dimmed as much as the first lamp's shade
  there. It is the lot ground's shade (outside walls only, at the lot's ground height, unverified as the ground under
  every piece), the lot is the one under the group's centre (a group crossing into a neighbouring lot is shaded by the
  first lot's mask, clamped at its edge), and a shader with no sampler or constants to spare keeps the copy of before
  (none in the precomp or the captures).
- A lamp with cones is gated per pixel by them, but roofs and water still take the lamp rows without cones.
- Groups whose instance positions cannot be read keep the atlas term: the light of the ground, without height or
  occlusion, so a balcony railing gets the ground light below it and an indoor stair railing near an outdoor lamp can
  glow.
- Requires the world atlas, so it needs *Street lamps light lots* and *Smooth ground light*.
- Only the instanced structure vertex shaders that match the pattern are handled; other instanced objects (grids, columns,
  repeated fences with per-instance positions) keep the game's lighting.

## Technical reference

### Why the game leaves fences dark

- The `vs_3_0` fence, railing, post and stair shader reads lamps in VS `c4..c7` / `c8..c11` = `VertexLightDirections` /
  `VertexLightColors` (rig `+0x90` / `+0xD0`, parameter ids `DAT_01158c74` / `DAT_01158c78`), not the three main slots
  (`LightDirections/Colors`, rig `+0x10/+0x50`). `FUN_006BA340` fills the vertex slots only with the excess:
  `vcount = min(4, n - 4)` from `list[count..]`. With four or fewer gathered lights they stay zero. It also drops
  `list[3]` when n = 4 (off-by-one at `0x006BA386`, `add eax, -4`; not patched).
- `FenceRenderManager` (`0x00A6C6F0` AddRailSceneModel, `0x00A6CBE0` AddPostSceneModel) adds every rail or post as an
  instance (0x60-byte record) of a `SceneModelArray` (0x340 bytes, constructor `FUN_006F9A10`, vtable `0x00FF99D8`),
  grouped by {room, rail/post, model, product, compositor}. One rig per group, at the centre of the group's box
  (`FUN_006F8930` / `FUN_006F8C70`), and one instanced draw per group.
- The room is queried at the group centre; in a roofed room the rig is in mode 0 (that room's lights plus four grey fill
  lights).
- Fences that close an area create a roofless room (`room+0x18 = 1`); `FUN_006C7CF0 -> FUN_006BAA70` sets rig mode 1
  (`rig+0x1D4`); lamps then come only from the room list (`FUN_006BBDE0 -> FUN_006BB2F0`), and `FUN_006BB270` accepts only
  lights with `light+8 == rig+0x1E0`. Street lamps are room 0, so never.
- Rooms in modes 0 and 1 leave the cells and are not refreshed at dusk (`FUN_006B58F0` walks only the cells).

### Vertex shader recognition (`ShaderPatches::IsInstancedStructureVs`, VS class 7)

`vs_3_0`; no `mova` and no relative addressing; inputs `POSITION1` (instance translation) and `POSITION2` (instance
rotation); outputs TEXCOORD1 and COLOR0; contains `mov oT1.zw, rW.xyxz` (world xz into TEXCOORD1.zw) and
`mad oC0.xyz, rX.w, c11, rY` (the last vertex light into COLOR0).

### Pixel shader patch (`ShaderPatches::PatchInstancedLamps`, `ps_3_0`)

With a TEXCOORD2 input (world normal xyz, world height w) the patch also adds the per-pixel object lamps
(`pixelLamps`: `lampParamConst = (valid spatial group, lamp strength, 0, 1e-4)`, then per lamp (position, 1/R^2) and (colour, spill)) and a
ground weight constant `(a, b, 1, 0)`, the atlas weight being `sat(N.y x a + b)`: `(1, 0)` with lamps, `(0, 1)` (every
face) in fallback mode. The first lamp parameter selects an `lrp` between spatial lamp light and native vertex light; valid groups select spatial light even when all lamps are rejected. The lamp strength is `SurfaceLampGain(night, forcaLuzPorPixelNosObjetos) x forcaNasCercas`.

Lamp cones (F8 10-57-41): after the lamp block, `coneConst = lampConst + 16` holds two rows per lamp, the cones
`c = (-s a, -s o)` of the game's laws (type 5 `0x006BC940`: two cones `a1 +0x1A0 / o1 +0x174`, `a2 +0x190 / o2 +0x170`,
`s = [0x011D11A0] = 5`, spill `+0x150`; type 4 `0x006BDA10`: one cone `a +0x170`, scale `+0x154`, offset `+0x158`, spill 0;
area lights, types 9 and 10: one cone `c = (axis, 0)` with the third axis `+0x180` normalised, spill 0),
filled by `ReadLamp` (`LotLightBridge::LampCone`). Each lamp's term is multiplied by
`S + (1 - S) sat(max(l.c1.xyz + c1.w, l.c2.xyz + c2.w))` (8 more instructions per lamp, `l` = pixel to lamp
normalised, `S` = the colour row's `.w`, the spill's luminance); an unused slot or a lamp with no cone has `S = 1`. One
upload of `1 + 4 x 8` rows covers both blocks. The patch needs `maxConst + 36 < 224` (was `+ 20`). The roof and water
lamp rows (`g_lampData`) are unchanged. The part below is the base patch:

Needs a COLOR0 input with `.xyz`, a TEXCOORD1 input, and exactly one `add rD.xyz, rS, vC` (COLOR0 in either operand, the
other a temp). Resources: sampler `E = maxSampler + 1` (refused at 15), `cA = maxConst + 1` (atlas mapping),
`cB = cA + 1` (`.x` strength), temp `T = maxTemp + 1`; refused if `maxConst + 2 >= 224` or `maxTemp + 1 >= 32`. The
TEXCOORD1 declaration is widened to `.xyzw`. Inserted before the `add`, which then reads `rT` instead of `vC`:

```
mad   rT.xy, vT1.zwzw, cA, cA.zwzw       ; atlas UV from world xz
texld rT, rT, sE                         ; ground light atlas
mul   rT.xyz, rT, cB.x                   ; x strength (SurfaceLampGain)
max   rT.xyz, rT, vC                     ; never below the game's vertex lights
```

#### Ground lamp shade

F7 2026-10-13 09-31-30, pixel (1847, 1131), draw #268 (VS `21B84C10`, PS `2BC528C0`, 37 instances at y 52.905): the wooden
curbs of a flowerbed behind a free-standing test wall stayed lit by the garden post on the other side, while the grass
and the paths beside them were shaded. Two causes: the group's one lamp list keeps a lamp that the piece nearest it or
either end sees (the group runs past the wall), and the atlas term on the tops (`sat(N.y x 1 + 0)`) was not multiplied by
the lot's ground lamp shade `M`, unlike the lot pass and the story-0 floors.

With the per-pixel lamps, after the final `max rT.xyz, rLA, rT` the patch appends the floors' block (`AppendGroundShade`,
[floors.md](floors.md#ground-lamp-shade-on-story-0-floors)) on `rT`: `rT.xyz *= 1 + gate (M - 1)`, with world x and z
from the lamp code's position temp (TEXCOORD1.zw) moved by `N.xz x rows[10]` (`cM + 2`.z, 0.6 m for fences; the
floors' rows leave it 0) and world y from TEXCOORD2.w, temps `rLA` and `rLB` (free after the
`max`). Mask sampler `sS = sE + 1`; rows `cM .. cM + 2` = `GroundShadow::FloorShadeRows` (xz scale 1) right after the
cones (first law, `coneConst + 16`) or after the two reach rows (`reachConst + 2`). Without the per-pixel lamps, or
without a sampler (s15 at most) or the 3 constants, the copy is byte for byte the copy of before (`shadeSampler = -1`).
TEXCOORD1 and TEXCOORD2 keep the game's `_pp` declarations, as the per-pixel lamps already read them (partial precision
is a hint; unverified on hardware that honours it). In the capture's reach copy: mask `s7`, rows `c50..c52`.

### Draw (`DrawInstanced`)

Reached from `OnDrawInner` when the VS is class 7, after the *Street lamps light lots* gate.

1. Off if `cercasComLuzDoChao` is off or the atlas (`LightmapSmooth::Atlas`) is not
   ready.
2. `PatchedFor(g_fencePs, "Fence/stairs", PatchInstancedLamps)`: one patched copy per game pixel shader.
3. Save `cA` and `cB`; bind the atlas to `sE` (`SamplerBind`, no mip filter); set `cA` to the atlas mapping and
   `cB = (SurfaceLampGain(night, forcaNasCercas), 0, 0, 0)`; swap the pixel shader; draw; restore. The game's vertex
   shader is kept. Counter: "fences/stairs: N" in the developer status line.
4. Ground lamp shade (a copy with `shadeSampler >= 0`): `FloorShadeBind` with the lot whose rectangle holds the group's
   centre (`FindLotAt`, any rotation) binds `LotMaskTexture(lot)` to `sS` and the 3 rows, and restores both after the draw.
   No group read, no lot there, or *Walls shade lamp light near the ground* off: the white 1 x 1 mask and gate 0 (the draw as
   before). Developer status: `on fences/stairs: N draws with their lot, M without it` after the floors' counters. F7
   adds a `ground lamp shade:` line under the `FENCE/STAIRS (instanced) lit by the mod` line (the lot, its ground height
   and the gate, or why there is none).

**Lamp pick memo** (`PickFenceLamps`, `FencePickMemo`). The group's lamp pick (`PickFenceLampsImpl`: the lamps, their
cones and reach rows, the highest atlas lamp head and the ground share) is kept in 1024 direct-mapped entries keyed by
the group's entry, the generation of its positions (moved at every read of them) and the law, with the rules of the
outdoor objects' pick memo: an entry is valid while the lamp list (`g_lampMemoGen`) and everything the wall and slab
test reads (`LevelLightShare::LampVisibilityState`, the same before and after the pick) are unchanged. A lamp shining
out of a house is tested on its own copy of the walls, a snapshot that is replaced and never changed; its generation is
part of `LampVisibilityState`, so picks with such lamps are kept too. A hit writes exactly what the pick would: the same rows, head
height, share, counters and candidate count. The memo follows the development switch `lamp_visibility_memo` and is not
used while F8 records the wall and slab tests or a light capture runs. Development builds pick one hit in 64 again on a copy and
compare it bit for bit; a difference turns the memo off for the session
(`[LotLightBridge] Fence lamp pick memo: a kept pick differed from a new one ...`). A world change empties it. The
developer roof status line counts `fence lamp picks: N reused, M computed` (with `(off: a kept pick differed)` after a
difference).

Offline (`tools/floor_shade_check`, read-only): the 116 pixel shaders the precomp pairs with an instanced structure
vertex shader all take both laws with the shade (at most 335 slots); the shaded copy is exactly the copy without it plus
the sampler declaration and the block, and native D3D9 accepts both; with the shade off the copy is byte for byte the
copy of the previous source over every `ps_3_0` of the precomp, the captures and the refused shaders (a one-off build of
both sources, not kept); the captured copies `PS_2BC528C0` (reach) and `PS_2B07C790` (first law) are found again;
native D3D9 pixels of a synthetic fence shader, ground light alone and one lamp alone, both laws, equal the sum x
`1 + gate (M - 1)` within 2 LSB at four heights. The extracted `DrawInstanced` (resource fixture) binds and restores the
rows, looks the lot up at the group's centre and counts the draws.

### Daytime response (`TerrainLightingPolicy::SurfaceLampGain`)

```
if night >= 1 or input invalid: return gain
return gain x LampDay(night)        // LampDay(n) = 0.03 + 0.97 n^2 (3 - 2n), 1 at n >= 1
```

The same function feeds the outdoor object lamp paths and snow on objects (`DrawSnowOnObject`).

### Captures

| Capture | Shaders | Observation |
|---|---|---|
| Railing m03 | `PS_21846848` (hash 964b282b), `VS_21849CA0` | VS `c4..c11` all zero |
| Stairs `LightProbe-escada` #89 | `PS_215B1440`, `VS_216C8910` | |
| Fenced-area fence m15 | | VS `c4..c11` zero; roofless room, rig mode 1 |
| Weak fence m32 | `PS_21AA4330` / `VS_21793C00`, 72 prims, textures `s8..s10` | One grey light: direction (0.997, 0.05, 0.06), colour 0.354 (rig mode 0) |
| Grids m33, m37 | 108 prims | VS `c4..c11` zero |
| Outdoor bench (F7, day) | `PS_3019F738.bin` (912 bytes, FNV-over-DWORDs `C0F5CCD7`), 3 instances, 160 prims | Lamp term `max(atlas x c13.x, COLOR0)`; VS `c8..c11` zero, so the lamp colour came from the atlas |

### Addresses (Steam 1.67.2)

| Address | What |
|---|---|
| `0x00A6C6F0` / `0x00A6CBE0` | FenceRenderManager AddRailSceneModel / AddPostSceneModel |
| `0x006F9A10`, `0x00FF99D8` | SceneModelArray constructor, vtable |
| `0x006F8930`, `0x006F8C70` | Group box and rig placement at the group centre |
| `0x006BA340`, `0x006BA386` | Rig slot distribution, off-by-one (not patched) |
| `0x01158C74` / `0x01158C78` | VertexLightDirections / VertexLightColors parameter ids |
| `0x006BBE70` | Room gather call (fenced yards), patched by `ObjectLightBridge` |
| `0x006F68C3` | Proposed binder thunk site for filtering by rig mode (EBX = model, ECX = rig); not used |

### Files

| File | Symbols | Role |
|---|---|---|
| `features/shader_patches.cpp/.h` | `IsInstancedStructureVs`, `PatchInstancedLamps`, `InstancedPatch`, `AppendGroundShade` | Recognition and patch |
| `features/lot_light_bridge.cpp` | `ClassifyVs` (class 7), `DrawInstanced`, `PatchedFor`, `g_fencePs`, `SetFenceGroundLight`; `PickFenceLamps` / `PickFenceLampsImpl`, `g_fencePick`; `FindLotAt`, `FloorShadeBind`, `g_fenceShadeLot` / `g_fenceShadeNoLot` | Dispatch; lamp pick and its memo; ground lamp shade |
| `features/ground_lamp_shadow.h` | `FloorShadeRows` | The shade's rows |
| `features/terrain_lighting_policy.h` | `SurfaceLampGain`, `LampDay` | Daytime response |
| `features/object_light_bridge.cpp` | `RoomGatherThunk` | Fenced yards on the CPU (mode 1) |

## Rejected approaches

- Filling the three main rig slots (rig boost, `RigCtorForce`): these shaders read only the vertex-light slots. Details in
  [history](../../history/night-lighting-fences.md).
- Forcing `rig+0x224 & 0x10` for fences and stairs: the bit is already set.
- A daytime endpoint of 0.25: replaced by 0.08 after gameplay feedback.

## See also

- [Validation](../../validation/night-lighting-fences.md)
- [History](../../history/night-lighting-fences.md)
- [Objects and rigs](objects-and-rigs.md), [Foliage](foliage.md), [Snow](snow.md),
  [World atlas and smoothed maps](world-atlas-and-smoothed-maps.md)
- [Engine: light objects and rigs](../../engine/light-objects-and-rigs.md)

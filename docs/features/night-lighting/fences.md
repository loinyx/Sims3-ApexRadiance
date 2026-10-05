# Fences, railings, posts and stairs

At night, fence rails and posts, railings and outdoor stairs near lamps are as bright as the lit ground next to them,
instead of staying dark while the grass around them glows. Snow lying on fence tops and stair tops follows the same
switch and strength. By day the lamp term fades to a small share, so sunlit fences keep the game's look. Part of
[Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 1.0.0. Subdued daytime lamp response: in development (PR #2) |
| Default | On |
| Menu | Lighting > Objects > *Doors, counters and fences* card |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`IsInstancedStructureVs`, `PatchInstancedLamps`), [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawInstanced`), [`features/terrain_lighting_policy.h`](../../../features/terrain_lighting_policy.h) (`SurfaceLampGain`) |

## The problem

The game draws fences, railings and stairs as instanced "scene model arrays", and their vertex shader takes lamp light
only from the rig's four "vertex light" slots. The game fills those slots only with overflow lights, beyond the first
four gathered, so they are almost always zero. One rig also serves a whole fence group from the group's centre, which can
fall inside a roofed room and then sees only that room's lights. The result: fences next to brightly lit ground get no
lamp light at all. See [engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## How Apex Radiance solves it

Apex Radiance patches the fence pixel shader by pattern so that its lamp term becomes the brighter of the game's vertex
lights and the world ground-light atlas at the pixel's world position. Every rail and post then receives the same lamp
light as the ground beside it.

1. **Recognise the instanced structure vertex shader** by its instance streams and output shape.
2. **Patch the pixel shader** once per game shader: read the ground light atlas at world xz, scale it, and take the `max`
   with the game's vertex lights (COLOR0) in place of the plain add.
3. **Draw with the atlas bound** and the strength set per draw; restore everything afterwards.
4. **Daytime response:** the strength sent to the shader is `SurfaceLampGain(night, strength)`. At full night it is the
   configured strength; at full day it is `0.08 x min(strength, 1)`; in between it moves linearly with the night level.
   The game's vertex lighting stays the lower bound, and sunlight, geometry, materials and atlas mapping are untouched.

There are two fence shader families in the game. The `vs_2_0` instanced "object rig" family shares its shaders with
bushes and is fixed by the foliage and rig changes ([foliage.md](foliage.md), [objects-and-rigs.md](objects-and-rigs.md)).
This page covers the `vs_3_0` instanced structure family.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Fences and stairs catch light | `cercasComLuzDoChao` | bool | on | | Ground light on fences, railings, posts and stairs, and on snow lying on objects and stair tops ([snow.md](snow.md)) |
| Fence brightness | `forcaNasCercas` | float | 100% | 25 to 200% | Multiplies the atlas term; 100% = the same light as the ground. Shown when the switch is on |

Both apply live (`LotLightBridge::SetFenceGroundLight`, every frame). The card disables these rows unless *Street lamps
light lots* and *Smooth ground light* are both on (the atlas exists only with both) and offers a button to turn them on.
The Lighting balance styles set `forcaNasCercas` to 67.5% (Subtle), 75% (Soft) or 100% (Natural).

## Compatibility and interactions

- Snow on fence tops (VS class 8, `PatchSnowCover`) and on stair tops (class 9, `PatchSnowRelief`) use the same switch,
  strength and daytime response ([snow.md](snow.md)).
- Fenced yards: lamps reach their rig through the CPU room-gather thunk at `0x006BBE70` in `ObjectLightBridge`
  ([objects-and-rigs.md](objects-and-rigs.md)).
- `PS_2673D758`, a fence and railing pixel shader, also matches the outdoor object patch; the instanced structure class is
  dispatched first.
- No game code is patched for the shader part; recognition is by instruction shape.

## Limitations

- The atlas is the light of the ground, without height or occlusion. A balcony railing gets the ground light below it,
  and an indoor stair railing near an outdoor lamp can glow. The atlas term applies whatever the rig mode.
- There are no per-pixel lamps on fences: faces turned towards a lamp are not brighter than the ground at their foot. A
  fence far from any lamp's ground pool stays dim.
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

### Draw (`DrawInstanced`)

Reached from `OnDrawInner` when the VS is class 7, after the *Street lamps light lots* gate.

1. Off if `cercasComLuzDoChao` is off or the atlas (`LightmapSmooth::Atlas`) is not ready.
2. `PatchedFor(g_fencePs, "Fence/stairs", PatchInstancedLamps)`: one patched copy per game pixel shader.
3. Save `cA` and `cB`; bind the atlas to `sE` (`SamplerBind`, no mip filter); set `cA` to the atlas mapping and
   `cB = (SurfaceLampGain(night, forcaNasCercas), 0, 0, 0)`; swap the pixel shader; draw; restore. The game's vertex
   shader is kept. Counter: "fences/stairs: N" in the developer status line.

### Daytime response (`TerrainLightingPolicy::SurfaceLampGain`)

```
kDaySurfaceResponse = 0.08
if night >= 1 or input invalid: return gain
day = min(gain, 1) x 0.08
return day + (gain - day) x night
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
| `features/shader_patches.cpp/.h` | `IsInstancedStructureVs`, `PatchInstancedLamps`, `InstancedPatch` | Recognition and patch |
| `features/lot_light_bridge.cpp` | `ClassifyVs` (class 7), `DrawInstanced`, `PatchedFor`, `g_fencePs`, `SetFenceGroundLight` | Dispatch |
| `features/terrain_lighting_policy.h` | `SurfaceLampGain`, `kDaySurfaceResponse` | Daytime response |
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

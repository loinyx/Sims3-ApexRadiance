# Walls

Outside walls near lamps are lit as brightly as the objects in front of them, and a lamp on one story lights the walls
of the stories above and below it without a straight cut at the floor line. Walls keep the game's shaders, textures,
sun and shadows; only the lamp light baked into each wall is scaled. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (present since 2.1.0, the first version in this repository). Daytime lamp term on walls: in development (PR #2) |
| Default | On, Brightness 200% |
| Menu | Lighting > Buildings > Buildings card, group *WALLS* (lamps of every story: Lighting > Stories) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawWallGain`), [`shaders/wall_lamp_table.h`](../../../shaders/wall_lamp_table.h), [`features/terrain_lighting_policy.h`](../../../features/terrain_lighting_policy.h); lamps of every story in [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) |

## The problem

Outside walls take their lamp light from one baked light map per story, read in sampler `s2`: an atlas of small strips,
one per wall piece. Two defects follow:

1. **Cut at the floor line.** Each story's room 0 only gathers that story's lamps, so a sconce on the upper story lights
   its own wall and the wall just below stays dark with a straight cut.
2. **Walls darker than objects.** The wall's lamp light is only its baked map times one constant, the result of a room
   solve with a small factor (k2 = 0.075), much dimmer than the rig lamps objects get. By day the game sets that
   constant to zero on some wall variants, discarding the baked lamp light entirely.

## How Apex Radiance solves it

Two Night Lighting parts act on walls; no wall shader is rewritten.

1. **Lamps of every story** ([level-light-share.md](level-light-share.md)) put the outdoor lamps of every story into
   every story's room 0 list and test them against the walls of the lamp's own story, so the light continues past the
   floor line.
2. **Wall gain** multiplies the baked lamp term of the 58 ExteriorWall pixel shaders for each draw. For each recognised
   draw, Apex reads the constant that scales the baked map, writes `native x Brightness + (1 - night) x min(Brightness, 1)
   x 0.08` into its `.x`, draws, and restores the constant. At full night this is exactly `native x Brightness`; by day a
   subdued lamp term is added so the baked lamp light is not discarded.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamps light walls | `paredesComLuz` | bool | on | | Off keeps the game's walls, by day and night |
| Brightness | `forcaNasParedes` | float | 200% | 25 to 400% | Lamp light on outside walls, by day and night. `LotLightBridge::SetWallGain` clamps to 25 to 800% |

Both apply live (pushed every frame). The lamps of every story are controlled by *Outdoor light between floors*
(`luzExternaEntreAndares`), documented in [level-light-share.md](level-light-share.md). The wall gain works with
*Street lamps light lots* off: `DrawWallGain` is dispatched before the bridge-enabled check in `OnDrawInner`, and
`UpdateHooks` keeps the draw hooks registered while the wall switch is on, including at 100%.

## Compatibility and interactions

- **Objects** ([objects-and-rigs.md](objects-and-rigs.md)): before the story share, a window's rig picked outdoor lamps
  of any story (room id 0 is 0 everywhere) while the wall around it did not. With the share both see the same lamp list;
  the rig still uses the 3 strongest lamps at the object centre without wall shadow, while the map sums every lamp per
  point with wall occlusion.
- **Sims3SettingsSetter Split-Level Lighting Fix**: type-11 lot lights enter every story through the world gather; the
  share handles types 3 to 6 as well.
- **Floors** of each story use the same per-story maps ([floors.md](floors.md)).
- **Roofs** have their own brightness in the same card ([roofs.md](roofs.md)).

## Limitations

- A balcony slab on the upper story does not block light going down: a room's blockers are its own story's walls.
- The gain scales everything baked in the wall map. The map is a lamp and room solve, so window light baked into it, if
  any, is scaled too (inferred).
- The gain adds no light where the game baked none: no new lamps, no change to wall occlusion.
- Wall variants outside the table (for example `PS_2A74E378`, if it is not one of them) keep the game's look.
- The daytime term is visual tuning, not a physical ratio. Brightness below 100% stays lower than the game.

## Technical reference

### How a wall is lit

LightProbe captures `andar2-b` (m44), `andar1-b` (m45), `passo3-parede` (m59) and m75:

- Wall VS/PS pair VS_2669D978 / PS_2669DA40 (1372 bytes, FNV-1a over DWORDs `0x04956FE9` = `ExteriorWall_PS_1119`, K = 3);
  m59's VS_2734A4D8 / PS_2734BB80 and m75's PS_29548148 are the same pixel shader, and so is daytime `PS_265EBE18`.
- Lamp light comes from `s2`, one light map per story (256x128 in m44/m45: T9 upper story, T3 ground; 1024x512 in m59).
  UV from the vertex: `o2.xy = TEXCOORD1 x 1/4096` (VS `mul o2.xy, c20.y, v3`).
- PS: `texld r1, v1, s2` then `mad r5.xyz, r1, c3.x, r2`. m59 also uses a normal map (s7), the sun shadow `texldp s5`
  (4096²), cubes s0/s1, texkill, about 70 instructions; camera in VS c11.
- By day the captured `c3 = (0, 0.188235313, 0, 0)` makes that `mad` discard all lamp RGB from `s2`.
- The class-2 wall blur (`room+0xF4`, `DAT_01158b1c`, `DAT_011d02e4`) runs over the wall atlas: `[1 2 1]/4` separable by
  default, a 2x2 box with a half-texel shift otherwise (`fn_0069f650.c`). Room solve factors (F8): k1 = 1, k2 = 0.075,
  Cmax = 2, street factor 3.333, type-5 s = 5, 2 blur passes, mode 0. `room+0x63C` is the per-light threshold of the
  solve (`0x0069FE19-0x0069FE4D`).
- Atlas alpha: room-0 texels always have alpha 0; indoor texels have alpha up to 1, used as a sky term by InteriorWall
  families.
- Wall VS in cutaway / walls-down mode: `max r1.xy, c12.xzzw, v7.xzzw`, `min r1.w, r1.x, c12.y`, `mad r2.y, r1.w, r0.w, r0.z`
  lower the wall (y = lerp(v0.w, v0.y, clamp(v7.x))), while the atlas UV is not lowered.

### Wall gain

- `wall_lamp_table.h` lists the 58 pixel shaders of the game's ExteriorWall technique (`Shaders_Win32.precomp`) with
  size in bytes, FNV-1a 32 over DWORDs and K, the constant whose `.x` scales the baked map: c2 in 26 entries, c3 in 32.
  The baked map is the `texld` at TEXCOORD1 (`s2` in the full variants, `s1` in the simple ps_2_0 ones) or, in 4
  variants, the single 2D `texld` (TEXCOORD3) read by `mad ..., cK.x`; K is read nowhere else. K is per variant because
  in 24 variants c3.x is the bloom threshold, not the lamp scale. No other wall family (InteriorWall, ExteriorWallAOSI,
  UnlitExteriorWall; 27 variants) matches. Generated offline (scratchpad `snowcover/test11.cpp`).
- `ClassifyPsCode -> WallLampConst(code, size)`: a table match gives `PsClass::WallGain` and K in `g_wallConst[ps]`.
- Per draw (`DrawWallGain`): off -> the game draws. Otherwise read `cK`; invalid or negative values, failed reads,
  unknown shaders and an unchanged result keep the native draw. `TerrainLightingPolicy::WallLampScale(native, night,
  gain) = native x gain + DayLampScale(night, min(gain, 1) x kDaySurfaceResponse)`, with `DayLampScale(n, g) = (1 -
  clamp(n, 0, 1)) x g` and `kDaySurfaceResponse = 0.08` (shared with objects, fences and snow on objects). Only `.x`
  changes; the full constant is restored after the single draw. Counter `g_wallDrawn`.

### Lamps of every story (summary)

Room lists are built by `FUN_006c7010 -> FUN_006c6ab0(treeLevel, room)`; `OutdoorGather` on its two callers
(`0x006C5816`, `0x006C7094`) adds the other stories' outdoor lamps, `0x006C73B1` JNZ -> JL refreshes all stories, and
the 9 light classes' `vfunc+0x4C` wrappers test borrowed lamps with `FUN_0069fc40` against room 0 of the lamp's story and
every story in between, mirroring the per-batch culling (`FUN_006a30b0 -> FUN_0069dff0`, batch vector `0x01158AC8`) and
swapping the wall mode byte `room+0x639`. Addresses and details: [level-light-share.md](level-light-share.md).

### Diagnostics (developer mode)

Developer page status *Walls*: `outside walls: strength 2.00 | draws: N | variants seen: M`; status *Stories* for the
share. F7 on a wall shows the PS (1372 bytes or another table entry) and the draw's multiplied c3.x or c2.x.

## Rejected approaches

- A per-pixel wall lamp model (PASSO3 design A) was designed and critiqued but never implemented; pattern patches tested
  against the whole shader package were chosen instead. Details in [history](../../history/night-lighting-walls.md).
- A signature cascade between stories, and a cross-story test without batch culling or the wall-mode byte: see
  [level-light-share history](../../history/night-lighting-level-light-share.md).
- A daytime wall term of 25% of the unboosted lamp scale: too saturated; reduced to 8%.

## See also

- [Validation](../../validation/night-lighting-walls.md)
- [History](../../history/night-lighting-walls.md)
- [Light between stories](level-light-share.md), [Room light maps](../../engine/room-light-maps.md),
  [Shaders](../../engine/shaders.md)

# Walls

Outside walls near lamps are lit as brightly as the objects in front of them, and a lamp on one story lights the walls
of the stories above and below it without a straight cut at the floor line. Walls keep the game's shaders, textures,
sun and shadows; only the lamp light baked into each wall is scaled. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released (present since 2.1.0, the first version in this repository). Daytime lamp term on walls: Released in 2.6.0. The game's bloom alpha kept on lit walls and the rebalanced style brightness (84% for Soft): Released in 2.7.0. Realistic lamp reach and balcony floors blocking a lamp below them (replacing *Light behind balconies*), the shared daytime curve: Released in 2.12.3. Soft edges by the lamp's size (*Shadow softness*) and lamp light through openings: Released in 2.13.0. Outdoor walls drawn below their light base (gables, eaves) lit from their drawn foot: Released in 2.15.3. Adjacent gable cells keep one lighting reference and upper trim avoids the ground lamp pool: Released in 2.15.4. Light past 1 on ps_3_0 outside walls, wall lamps lighting their own wall and the wall shade strip left as the game maps it: Released in 2.16.0. |
| Default | On, Brightness 30%, Realistic lamp reach on |
| Menu | Lighting > Outside > Buildings card, group *Walls* (lamps of every story: Lighting > Inside & floors > Stories) |
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

Raised short gable cells keep their sample rows and vertex UVs on one drawn-foot reference, including neighboring cells with different top heights.

Three Night Lighting parts act on walls.

1. **Lamps of every story** ([level-light-share.md](level-light-share.md)) put the outdoor lamps of every story into
   every story's room 0 list and test them against the walls of the lamp's own story, so the light continues past the
   floor line. A floor a player placed on an upper story (a balcony, a deck) blocks a lamp below it: the wall behind a
   balcony gets nothing from a lamp under the slab, while a lamp in front of the balcony still lights it, with a soft
   edge where its light passes the slab's edge as wide as the lamp's ball (*Shadow softness*) makes it there.
2. **Wall gain** multiplies the baked lamp term of the 58 ExteriorWall pixel shaders for each draw. For each recognised
   draw, Apex reads the constant that scales the baked map, writes into its `.x` `native x Brightness` at full night
   (unchanged) and `Brightness x LampDay(night)` before it (the shared daytime curve, 3% of the night strength by day,
   in place of the game's own daytime factor), draws, and restores the constant. Until 2.12.2 the daytime value was
   `native x Brightness + (1 - night) x min(Brightness, 1) x 0.08`.

   The ExteriorWall shader derives its output alpha, the scene's bloom mask, from the final luminance, so a stronger lamp
   term would also widen the bloom over whole walls and foundations. For opaque wall draws Apex therefore draws twice:
   an alpha-only pass with the game's own constant (no depth or stencil write) writes the game's bloom alpha, then the
   RGB pass with Apex's gain and alpha writes off. Blended wall variants keep the single pass. Found and validated by
   idavidveiga ([day-bloom-fixes.md](day-bloom-fixes.md)).

   With Instant lamp lighting the RGB pass of the 27 ps_3_0 variants is drawn with a copy that multiplies the wall
   atlas read by the relit atlas's outdoor scale (`ShaderPatches::PatchWallScale`: right after the map's `texld`, the
   same uv read again from a spare sampler, `x 2^(3 r)`), so a wall beside a strong lamp shows the light past 1 as the
   ground below it does ([rt-outdoor-lamps](rt-outdoor-lamps.md)). The bloom pass keeps the game's shader; the 31
   ps_2_x variants keep the cut at 1.
3. **A lamp on a wall lights that wall.** A sconce a few centimetres in front of its wall reaches the wall above it at
   5 to 10 degrees (N.l 0.1 to 0.15), and the wall stayed dark beside a perpendicular wall lit head on. On a wall texel
   (|n.y| < 0.5) with the lamp in front of its plane within 0.4 m, N.l is at least `0.5 x (1 - plane / 0.4)`, the light
   a real wall shows over its sconce: in the GPU relight's law and its CPU model (`GpuLampLaw::MountedNdl`) and in the
   game's evaluation in the room solves (the colour scaled by the raised N.l over the game's). Lamps farther than 0.4 m
   from a wall and every other surface keep the game's law.

### Drawn wall sample positions

Outdoor gable and facade cells use measured geometry and the native UV endpoints to align their sample columns.
Clipped ends interpolate only between compatible anchors; unknown or conflicting geometry keeps the native mapping.
The vertical base follows the drawn cell, including a tip clipped below its native foot. The normal perpendicular
sample bias is preserved. This changes sample alignment, not lamp strength or reach.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamps light walls | `paredesComLuz` | bool | on | | Off keeps the game's walls, by day and night |
| Brightness | `forcaNasParedes` | float | 30% | 25 to 400% | Lamp light on outside walls, by day and night. `LotLightBridge::SetWallGain` clamps to 25 to 800% |
| Realistic lamp reach | `alcanceRealistaDasLampadas` | bool | on | | Lamp light on outside wall texels is kept up to 3.5 m from the lamp, then falls as (3.5 / d)^2 more inside a smooth window ending at 15 m, for a range 42.5 x intensity 2 post (both scaled with sqrt(range x intensity): 2 to 5 m, 6 to 20 m). Replaces the upper-story distance share and the steep-ray dimming of upper-story walls (`UpwardShare`: F7 20-31-19 / 20-31-21 showed it alone left story 3 dim; the slabs between stories and this law cover what it stood for). While it is on, the per-pixel lamps of outdoor objects, fences and windows follow the same law at the walls' scale ([objects-and-rigs.md](objects-and-rigs.md#realistic-reach-on-objects-fences-and-windows)). Relights every room when changed (`LevelLightShare::SetRealisticReach`) |

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

- Final release-build gameplay confirmation is pending; offline checks do not establish identical material appearance or frame-time cost.

- A balcony slab blocks only light going up: a lamp on a balcony still lights the wall under it, and story planes use
  the story's nominal floor height (split levels may be off).
- The wall behind a balcony gets nothing from a lamp under the slab; light bouncing off the slab or the ground is not
  modelled, so with no other lamp in view that wall keeps only the game's ambient light.
- The gain scales everything baked in the wall map. The map is a lamp and room solve, so window light baked into it, if
  any, is scaled too (inferred).
- The gain adds no light where the game baked none: no new lamps, no change to wall occlusion.
- The mounted-lamp floor is visual tuning for wall lamps, not a measured ratio; in the room solves the game's Cmax cut
  of a lamp touching the wall is not redone after it.
- Gables, eaves and sloped foundation faces lit from their drawn foot are not yet confirmed in game.
- Wall variants outside the table (for example `PS_2A74E378`, if it is not one of them) keep the game's look.
- The daytime curve is visual tuning, not a physical ratio. Brightness below 100% stays lower than the game.
- The curve meets the night value only if the game's own factor is 1 at full night (F7 20-31-19 on the captured
  variant; other variants unverified).

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
- Light rows on the wall mesh: the light-UV writer `0x00C38530` (called per wall cell at `0x00C387B5` / `0x00C387CC`, mode 0
  / 1 = the V at vertex `+0x12` / `+0x1E`, x 4096; mode 1 is TEXCOORD3, the game's wall shade strip (PS `s6`, L8 1024 x 32),
  which Apex leaves as the game maps it: only mode 0's light rows are recut) maps the bottom vertices to the light base and the top ones to
  base + 3 (1 - f), `f = max(0, int16 +0x0A / 512 - 8)` from the top vertices' wallpaper V (`0x00C385AB..DB`; the wall
  builders write `+0x0A = (V + 8) x 512` at `0x00C3C587` / `0x00C3C8B9`). Walls taller than 3 m get their light samples
  stretched (`StretchTallWall`). Short walls (F8 10-57-41): on a foundation face (story 0, drawn from the
  terrain to the foundation's top, about 0.75 m) that V is about 0, so the game squeezed the whole 3 m block onto it and
  the top of the stone showed light computed well under a sconce. With *Wall lamps at the right height*
  (`paredesExternasNaAlturaDesenhada`), `WallUvHook` writes a short cell (under 2.98 m) whose game mapping is more than
  2 cm off its drawn heights untrimmed, then sets each vertex's V to the row lit at its own height (`clamp((y - base) / 3)`, rows at
  base + 3k/(N-1)); the trim words go back. Counter: "short wall cells (foundation faces) lit at their drawn heights";
  dev log `[WallTall] short wall lit at its drawn heights`.
- Walls drawn below their light base (2.15.3). An outdoor room's wall is lit from story x 3 (or the floor under it), but a
  gable is drawn from the top of the knee wall below it: in two light captures story 4's gable was drawn from 27.822 with
  its light base at 29.708, so its lower 1.9 m took one row lit 1.9 m above it, a hard step where the gable starts. A
  garage beside a house on a foundation showed the same under its eave (drawn 0.76 m below its base). Per cell, an
  outdoor wall drawn 5 cm to 3.2 m below its light base has its sample columns moved down to the cell's drawn foot after
  alignment and stretching (`DropLowFootWall`), and the cell is recut against that foot (`WallUvHook`), so each vertex
  reads the row lit at its own height; the gable's eave cell, whose tip dips under its foot, is recut like its
  neighbours. That relaxed recut applies only to walls drawn below their light base; other cells are recut only when
  one height is over 0.2 m and none is under it. A cell seen low for the first time sends its room to be solved again (`NoteWallCell`); a column of a cell
  not drawn yet keeps the base. The light probe finds a wall's pixels down to its lowest lit row.

### Wall gain

- `wall_lamp_table.h` lists the 58 pixel shaders of the game's ExteriorWall technique (`Shaders_Win32.precomp`) with
  size in bytes, FNV-1a 32 over DWORDs and K, the constant whose `.x` scales the baked map: c2 in 26 entries, c3 in 32.
  The baked map is the `texld` at TEXCOORD1 (`s2` in the full variants, `s1` in the simple ps_2_0 ones) or, in 4
  variants, the single 2D `texld` (TEXCOORD3) read by `mad ..., cK.x`; K is read nowhere else. K is per variant because
  in 24 variants c3.x is the bloom threshold, not the lamp scale. No other wall family (InteriorWall, ExteriorWallAOSI,
  UnlitExteriorWall; 27 variants) matches. Generated offline (private research folder: `snowcover/test11.cpp`).
- `ClassifyPsCode -> WallLampConst(code, size)`: a table match gives `PsClass::WallGain` and K in `g_wallConst[ps]`.
- Per draw (`DrawWallGain`): off -> the game draws. Otherwise read `cK`; invalid or negative values, failed reads,
  unknown shaders and an unchanged result keep the native draw. `TerrainLightingPolicy::WallLampScale(native, night,
  gain) = native x gain` at night >= 1, else `gain x LampDay(night)` with `LampDay(n) = 0.03 + 0.97 smoothstep(0, 1, n)`
  (the curve every outdoor lamp term shares, [README](README.md#daylight-policy-terrain_lighting_policyh)). Only `.x`
  changes; the full constant is restored after the single draw. Counter `g_wallDrawn`. The last wall shader's `K` is
  kept between draws, and the native scale read here (`NativeWallScale`) is stored without a locked exchange
  ([per draw](README.md#per-draw-lot_light_bridgecpp)).

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

- Recutting only a gable's eave cell at its drawn heights: reverted after broken light in game; the whole wall is
  lit from its drawn foot instead ([history](../../history/night-lighting-walls.md)).
- A per-pixel wall lamp model (PASSO3 design A) was designed and critiqued but never implemented; pattern patches tested
  against the whole shader package were chosen instead. Details in [history](../../history/night-lighting-walls.md).
- A signature cascade between stories, and a cross-story test without batch culling or the wall-mode byte: see
  [level-light-share history](../../history/night-lighting-level-light-share.md).
- A daytime wall term of 25% of the unboosted lamp scale: too saturated; reduced to 8%, then (2.12.3) replaced by the
  shared 3% curve: the 8% term was still a visibly lit wall next to a lamp by day.
- A balcony share (*Light behind balconies*) lit the wall behind a slab from a lamp beneath it (captures
  18-38-41): see [level-light-share history](../../history/night-lighting-level-light-share.md).

## See also

- [Validation](../../validation/night-lighting-walls.md)
- [History](../../history/night-lighting-walls.md)
- [Light between stories](level-light-share.md), [Room light maps](../../engine/room-light-maps.md),
  [Shaders](../../engine/shaders.md)

# Light through arches and doorways

The floor that runs through an arch between two rooms has no colour step on the arch's line, and the seam does not come
back after the camera changes stories. A lamp's light through an arch or door into the next room has a soft edge but
keeps the opening's shape. Light through openings no longer changes when the camera moves. Part of
[Night Lighting](README.md); it builds on *Indoor light through openings* ([level-light-share.md](level-light-share.md))
and [Smooth light maps](room-map-smoothing.md).

## Status

| | |
|---|---|
| Availability | Released in 2.14.0. The story memo of 16 room maps and the kept segments of the matched rooms: Released in 2.15.3. Direct map-slot validation and room shader state batching: Released in 2.15.6. |
| Default | On |
| Menu | None. Follows Lighting > Buildings > Rooms at Night > *Indoor light through openings* and *Smooth light maps* |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml`, read through `luzInternaPelasAberturas` and `mapasDosComodosSuaves` |
| Source | [`features/arch_ambient.h`](../../../features/arch_ambient.h), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`PatchRoomMapSmooth`, arch variant), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (`InnerPass`, `GridPassImpl`, `GridEdgeSpot`, `kFloorSill`, `InnerNormScaleAt`) |

## The problem

- **Colour step on the arch's line.** The floor shader draws `map.rgb + map.a x c4`, and `c4` is each room's own ambient
  (`room+0x110`, [shaders.md](../../engine/shaders.md)). Two rooms joined by an arch share one light map but not one
  ambient, so the colour jumps exactly on the arch's line.
- **A sill at floor level.** An arch's sill (height 0) had the same soft rim as a window's sill, which darkened the floor
  just past the arch by about 25%.
- **Normalisation from the previous solve.** Light imported through an opening is scaled by the source room's
  normalisation over the receiving room's. The receiving room's value is set only at the end of its own solve, so samples
  read the previous one: right on entering a lot, wrong after a floor switch.
- **A one-texel edge.** The cone of a lamp through an arch went from none to full within one 25 cm texel (a step of 0.81
  between neighbouring texels).
- **Soft tops under a slab.** A full wall under the floor above had a soft top, so light passed over a wall that meets
  the slab.
- **Camera-dependent light.** Indoor light through openings tested a set of stair and railing triangles collected from
  the screen. That set followed the camera, and every change relit the rooms near openings.

## How Apex Radiance solves it

1. **Arch ambient in the floor shader.** Each floor pixel near an arch adds
   alpha x max(0, neighbour's ambient - own ambient) x weight, with weight 1 on the arch's line and 0 at 1.5 m. Both
   rooms do the same, so on the line both show the brighter ambient and neither side darkens. The draw finds its room by
   matching `c4` within 2% plus 1e-4 per channel (`ArchAmbient::Same`), narrowed to the draw's story; rooms of that
   story with the same ambient give their arches together. Up to 6 segments are passed per draw.
2. **Floor-level sills.** A sill 2 cm high or less has no soft rim; the opening starts at the floor. Window sills keep
   theirs. This rule has no switch.
3. **Normalisation settling.** Imports into a room of the same story use the normalisation that room's last solve ended
   with, read live after the atrium merge. A solve that ends with another value sends the room once more, at most 3
   times in 10 s.
4. **Soft cone edges.** The edge of a lamp's cone through an arch or door is spread over 0.3 m at the receiving point.
   The middle of the cone keeps its full light and the opening's shape stays recognisable.
5. **Hard wall tops under a ceiling.** In the grid walk, a full wall whose top reaches the ceiling of a roofed tile
   (within 5 cm) has a hard top.
6. **Steady indoor light.** Light through openings and between stories no longer tests the screen-collected mesh set,
   and that set is no longer collected. After each lamp edit, once the lamps have been still for 400 ms, the rooms near
   openings on every lot are sent again once.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| No setting | | | | | On whenever *Indoor light through openings* (`luzInternaPelasAberturas`) and *Smooth light maps* (`mapasDosComodosSuaves`) are on |

## Compatibility and interactions

- The arch segments share the floor shader's constant range with the smoothing constants
  ([room-map-smoothing.md](room-map-smoothing.md)).
- [Rooms at Night](unlit-rooms.md) sets the ambients that step 1 blends.
- [Light through stairwells](stairwells.md) uses the same normalisation settling for rooms lit from another story.

## Limitations

- Not confirmed in game.
- Floors only: on the wall atlas the lot position cannot be derived from the uv, so walls keep the step.
- A change of the Rooms at Night slider reaches the arch on the rooms' next solve.
- The hard shadow that stair treads cast under a stair is no longer drawn.
- The receiving room follows its own normalisation, not later changes of the source room's.
- A floor draw takes at most 6 segments; segments beyond those are dropped for that draw.

## Technical reference

The room-map memo retains the native slot and wrapper that proved the map owner. Each reuse validates the slot,
wrapper, type and texture directly; a mismatch falls back to the full manager scan. Room ambient values remain live.
Room-map drawing saves a small contiguous constant block and restores it after the draw, preserving the intervening
rows. Failed block operations retain the individual-row fallback. No ambient intensity or shader equation changes.


| Item | Value |
|---|---|
| Arch reach | 1.5 m (`kReach`, `kArchReach`) |
| Segments per draw | 6 (`kArchCells`), 2 constant rows each: (a.x, a.z, e.x, e.z), (D.rgb, 1 / \|e\|^2) |
| Room match | `c4` within 2% + 1e-4 per channel; the draw's story from its room map set (manager `+0x16C` + class x 64, unverified in game), else from its directional maps (`+0x220`) |
| Story memo | The stories of the 16 most recent room maps (`ArchAmbientConsts`, one slot per map by pointer hash), each answer checked against its retained native slot, wrapper, type and texture; a mismatch scans the manager's sets (a map is in one manager's sets only, so a confirmed entry is the manager the walk would find); a miss walks the rooms with arches (`StoryMapsInclude`). A frame's floors alternate between the stories' maps, so one remembered map often missed |
| Segment memo | The matched rooms' 1 m cells joined into segments (quadratic in the cells) are kept for the last 8 sets of rooms while `g_archGen`, which every writer of the arch records moves under the same lock, is the same; the neighbours' ambients are still read live. The development draw cost log times the floors' arch rows |
| Floor sill | 0.02 m (`kFloorSill`) |
| Cone edge spread | 0.3 m (`kInnerSoftSpread`) |
| Wall top tolerance | 0.05 m |
| Settling | at most 3 resends in 10 s; `RoomSolveStartHook` records the value used |

The developer F8 snapshot counts floor draws given a neighbour's ambient, draws of no known room, settled and resent
normalisations and lamp edits that sent the rooms near openings again, and lists the last unmatched floor draws near a
room with an arch with both ambients.

## Rejected approaches

- Baking the arch ambient into the packed texel: the ambient during the solve is not the one drawn.
- An exact float match of `c4`: small differences dropped the transition.
- Specular as the cause of the seam: both sides use the same constants and textures.
- A larger lamp radius against the cone edge: the step stayed at 0.81.
- A 0.5 m cone spread: it blurred the shape of arches and windows. 0.15 m changed nothing.
- Testing indoor light against the screen-collected mesh set: the light followed the camera.

Details in [history](../../history/night-lighting-arches.md).

## See also

- [Validation](../../validation/night-lighting-arches.md)
- [History](../../history/night-lighting-arches.md)
- [Light between stories](level-light-share.md), [Smooth light maps](room-map-smoothing.md),
  [Light through stairwells](stairwells.md)
- [Engine: room light maps](../../engine/room-light-maps.md), [Engine: shaders](../../engine/shaders.md)

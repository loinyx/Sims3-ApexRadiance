# Light through stairwells

A lamp below a stairwell lights the walls of the story above through the opening in the floor, and the edge of that
light is soft instead of a straight line. The landing and the rooms above also take the light the lit surfaces below
reflect up through the opening. A room lit only from another story is brought up partly, the way the eye adapts to a
dark room, and never as bright as a room with its own lamp. A lamp in a closed room next to the stairwell no longer
leaks through its walls onto the story above. Part of [Night Lighting](README.md); it builds on
[Light between stories](level-light-share.md).

## Status

| | |
|---|---|
| Availability | Released in 2.14.0 |
| Default | On |
| Menu | None. Follows Lighting > Stories > *Indoor light between floors* (which needs *Outdoor light between floors* and Night Lights) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml`, read through `luzInternaEntreAndares` |
| Source | [`features/level_light_share.cpp`](../../../features/level_light_share.cpp), [`features/stairwell_bounce.h`](../../../features/stairwell_bounce.h), [`features/opening_bounce.h`](../../../features/opening_bounce.h), [`features/penumbra.h`](../../../features/penumbra.h) |

## The problem

With *Indoor light between floors*, a lamp shines into the story above or below through a floor opening
([level-light-share.md](level-light-share.md)). Four defects remained around stairwells:

- **Leaks through walls.** The part of the ray inside the lamp's own story was tested only by the game's soft 2D wall
  test, which shades a ray only near the end of a short wall. A lamp in a closed room beside the stairwell, hanging just
  under the slab, lit the walls of several rooms above through 2 m walls.
- **Hard slab edges.** The light that passed the opening ended at the projection of the slab's edge in a sharp line.
- **Black upper story.** A floor beside the opening lies in the plane of the opening and cannot see a lamp below it.
  Without a bounce term, the story above stayed black except where direct light reached its walls.
- **No adaptation.** The game sets a room's normalisation from its brightest sample or lamp
  ([room-light-maps.md](../../engine/room-light-maps.md)). It counts a lamp of another story at full strength even when
  the slab blocks almost all of it, so a room lit only through the opening never adapts.

## How Apex Radiance solves it

Direct light keeps the game's lamp functions and only gains stricter tests and a soft edge. Bounce light is computed
from physical quantities (an albedo of 0.4 for interior floors and walls and the game's own lamp irradiance), so it
adds no light that the lamps below do not send.

1. **Walls of the lamp's story.** The segment of the ray inside the lamp's story is walked on that story's wall grid,
   and the crossing test is hard (`GridWallHitHard`). A full wall counts up to the floor above, so a ray that crosses
   it a few millimetres under the slab is stopped. A half wall uses its own top. This is a veto only: a ray through a
   real opening keeps its previous result.
2. **Soft slab edges.** The light through a floor opening is the share of a 0.25 m lamp disc that the slab's edge
   leaves open (`PenumbraShare`). On the floor below this gives about 0.7 m of transition.
3. **Stair height.** For a lamp on the story above, the directional maps of the tiles under the upper floor's opening
   are sampled 1.5 m higher, where the upper steps of the stair stand.
4. **Shared ambient.** A room joined to another story by an opening too small to form an atrium takes
   0.5 x (opening quadrants / room quadrants) x the other room's ambient, converted to its own normalisation. The share
   is capped at the brighter of the two ambients and never darkens a room. It uses the ambient the game computed for
   the other room, so there is no feedback. There is no extra band of light at the opening's edge: a lamp below the
   floor's plane cannot reach its top face.
5. **Second bounce on the walls above.** A wall of the upper story that faces the opening sees, through it, the floor or
   wall of the room below. That surface reflects albedo x irradiance / pi, and each opening patch adds
   radiance x cos x cos x area / r^2 to the wall texel. The irradiance comes from the game's own lamp functions.
6. **Ceiling bounce onto the upper floor.** Each opening patch sends its reflected flux up along 24 rays in 3 rings
   (20, 45 and 70 degrees). The ceiling returns it to the floor over two legs of 3 m. A ray that falls into the well of
   a stair meets the stair 1.5 m above the floor below.
7. **Room fill.** A room lit only from another story takes back, on its floor and walls, albedo x the mean light its
   texels received in its last solve. The room is solved again when that mean moves by more than 25%, at most twice in
   10 s, so the fill follows a lamp that is switched off.
8. **Partial adaptation.** In a room lit only from another story, the normalisation is
   sqrt(1 / the brightest texel measured in its last solve), kept between the game's value capped at 1 and x2. A room
   that receives 5% of full light becomes twice as bright, not twenty times. A room whose brightest texel takes under 1%
   is brought up less, and not at all under 0.2% (`kAdaptNoneTook`, `kAdaptFullTook`): the faint residue of a far lamp
   stays dark, as the eye sees it.

Steps 7 and 8 apply only to rooms whose lamps all come from another story. Rooms lit through doors and windows keep
the game's behaviour, so the shape of the light through those openings is preserved.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| No setting | | | | | On whenever *Indoor light between floors* (`luzInternaEntreAndares`) is on |

## Compatibility and interactions

- Parent options: *Indoor light between floors* and *Every floor in full detail* (Lighting > Stories). With the first
  off, none of these steps runs.
- [Rooms at Night](unlit-rooms.md) supplies the room ambients that step 4 shares.
- Rooms joined by an opening larger than 30% of the smaller room are merged into one atrium (`MergeStackedAmbient`,
  [level-light-share.md](level-light-share.md)); the shared ambient of step 4 applies only below that size.
- [Light through arches and doorways](arches.md) shares the normalisation settling that makes step 8 converge.
- [Stairs on the directional maps](stairs.md) reads the per-tile ambient, so stairs follow step 4.

## Limitations

- Not confirmed in game.
- A dark wooden floor on the upper story stays dark: it receives only reflected light, and dark wood reflects little.
  This is physically correct.
- Steps 4 and 5 may count some of the same light twice; the overlap is not measured.
- The second bounce evaluates the lamps of the room below without Apex's wall and slab tests and may count an outdoor
  lamp of the ground story twice. The precision of the line of sight through the well is not verified.
- Rooms lit through doors or windows are excluded from fill and adaptation on purpose.
- The first solve of a room has no measurement yet; the room settles on the next solve.

## Technical reference

| Hook or function | Role |
|---|---|
| `SolvePoint` (`StairwellBounceAt`) | Second bounce, ceiling bounce and room fill per texel; records each texel's light before normalisation |
| `RoomNormHook` | Partial adaptation (`StairwellBounce::BorrowedAdapt`) for rooms whose lamps are all on another story (`CrossStoryOnly`) |
| `FinalizeHook` (solve end) | Fill check (`FillChanged`); `WallPassHook` is the fallback when the end hook is not installed |
| `IndoorPassImpl`, `GridWallHitHard` | The lamp's own story on the wall grid |
| `BounceThroughOpenings` | Shared ambient through openings (`opening_bounce.h`) |

| Constant | Value |
|---|---|
| `kSlabLampRadius` | 0.25 m |
| `kStairSampleUp` | 1.5 m |
| `kAlbedo` | 0.4 |
| `kWellLift` | 1.5 m |
| `kBorrowedAdaptMax` | 2 |

For walls more than 2 m from a tile of the opening, the whole tile is one source instead of four quadrants, and the
line below is walked in 0.5 m steps. The developer F8 snapshot counts rooms, texels seen and lit, the time spent, the
rooms adapted (game value, brightest texel, value kept) and the rooms sent again for a changed fill.

## Rejected approaches

- A veto that cut only rays with no light left: the wall top's soft rim let them through.
- A band of the lower room's ambient on the floor beside the opening: physically inverted.
- A stronger bounce from the opening's flux: it would invent light.
- Full adaptation up to x4, then adaptation by the game's own normalisation: rooms below a lamp reached x4.
- A walls-to-floor term for the upper room only: replaced by room fill.
- Fill and adaptation for rooms lit through doors or windows: they spread the light and lost the openings' shape.

Details in [history](../../history/night-lighting-stairwells.md).

## See also

- [Validation](../../validation/night-lighting-stairwells.md)
- [History](../../history/night-lighting-stairwells.md)
- [Light between stories](level-light-share.md), [Stairs on the directional maps](stairs.md),
  [Light through arches and doorways](arches.md)
- [Engine: room light maps](../../engine/room-light-maps.md)

# Light between stories

Lamps light every story of a house, not only the story they stand on. An outdoor sconce on the upper wall also lights the
wall below it, with no straight cut at the floor line, and garden lamps keep lighting the upper stories. Indoor lamps
shine through stairwells, atriums and removed floors into the rooms above and below, and the walls of a double-height
room meet at the floor line without a step in the light. Lamps never shine through a solid floor or around the walls of
their own story. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Missing outdoor gather entries restored: Released in 2.15.5.  Outdoor and indoor light between stories: Released (present since 2.1.0, the first version in this repository). Indoor light through openings across more than one floor, the raised-room wall veto, structure-change refresh and the *Wall seams.csv* recording: Released in 2.6.0. Walls block light on floors and objects, indoor objects lit by lamps of another story and an atrium's stories changing together, and the lit story's wall test from where an outdoor lamp's ray enters it (upper half walls): Released in 2.7.0. Placed floors of balconies and upper stories block a lamp below them (walls, floors, windows, objects, fences) and *Buildings block light on objects* (was *Walls block light on objects*): Released in 2.12.3. Lighting corrections: Released in 2.12.1. Outdoor light through openings into roofed rooms (garden lamps of the story below included), indoor light through openings, *Doors block light*, lamp light through openings in the outside walls' copies and room 0's own wall test, *Walls shade lamp light near the ground*, *Buildings block lamp light on trees and objects*, *Shadow softness*, walls ending at a floor above: Released in 2.13.0. Closed-floor support-row guards, walls of the lamp's own story for light through floor openings, hard wall tops under a ceiling, floor-level sills without a soft rim, and indoor light independent of the camera: Released in 2.14.0. *Indoor light adds to the ground*, pane-sized windows, ceiling lamps kept out of closed rooms above, story boundaries at tile floors and gables sampled from their drawn foot: Released in 2.15.3. Adjacent gable samples stay consistent; short pillar blockers use their measured top, and verified floor tiles under emitters cast per-lamp soft shadows on instanced structures: Released in 2.15.4. Per-key outdoor queue recovery and exact lamp evaluation optimisation: Released in 2.15.6. |
| Default | On (every switch; *Light through doors and windows* follows `kRealisticOpeningsDefault` in `build_flavor.h`, on) |
| Menu | Lighting > Stories (*Wall lamps at the right height*, *Seamless walls between floors* and *Every floor in full detail* under the folded *Game fixes*); Lighting > Buildings > Rooms at Night (group LAMP LIGHT: *Outdoor light through openings*, *Indoor light through openings*, *Indoor light adds to the ground*, *Doors block light*; then *Shadow softness*, and *Smooth light maps* under *Game fixes*); Lighting > Objects (*Buildings block lamp light on trees and objects*); Lighting > Ground (*Walls shade lamp light near the ground*) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/level_light_share.cpp`](../../../features/level_light_share.cpp), [`features/lamp_visibility.h`](../../../features/lamp_visibility.h), [`features/lamp_mark_filter.cpp`](../../../features/lamp_mark_filter.cpp), [`features/room_ambient_policy.h`](../../../features/room_ambient_policy.h), driven by [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) |

## The problem

Walls and floors of a lot are lit by a baked light map per room and per story (the room light solve, see
[room-light-maps.md](../../engine/room-light-maps.md)). The outside of each story is room 0, and the game builds room 0's
light list separately for every story. In the unmodified game:

- A sconce on the outside wall of the upper story lights the upper wall and nothing below the floor line: a straight cut.
- Garden lamps of the ground story light the upper stories only until the first update of that story's room 0.
- Windows and doors are lit by object rigs that compare only the room id (room 0 has id 0 on every story), so a lamp can
  light the window of one story and not the wall around it.
- An indoor room only ever receives lamps registered in that room. A lamp beside a stairwell lights its own story and
  stops at the moulding; the room below stays at its ambient colour.
- A double-height room is two rooms with separate ambient colours and separate wall sampling, so its walls show a seam at
  the floor line even when the lamps are shared.

## How Apex Radiance solves it

Short pillar blockers use the measured wall top. Foundation adjacency does not shorten a separate pillar. Complete floor tiles immediately beneath an emitter can supply per-lamp projected shadows for instanced structures; the bounded cache is invalidated by slab snapshots and floor edits.

Apex Radiance adds lamps of other stories to the game's own room light lists, with the same weight the game gives them
on their own story, and tests every borrowed lamp against the floors and walls it must pass, with the game's own wall
test. The light solve itself, lamp ranges and attenuation are the game's.

1. **Outdoor sharing.** After the game gathers room 0 of a story (0 to 7), the outdoor lamps of every other story are
   added with the game's gather function.
2. **Refresh cascade.** A change in room 0 of any story refreshes room 0 of all stories, so switching an upper lamp also
   updates the stories below. Exterior wall participants are included in the generation barrier. This does not guarantee that every story visibly changes at the same instant. A deferred
   request already covered by a newer completed generation does not repeat that work; removing a lamp still invalidates
   stale work safely.
3. **Walls of the lamp's story.** A borrowed outdoor lamp is tested against the walls of its own story and of every
   story in between, mirroring the game's per-batch wall culling. A porch lamp under an upper overhang is not affected.
4. **Indoor openings.** An indoor room near a removed floor takes the lamps of rooms on other stories that are near the
   same opening. Every point of the light map is lit by such a lamp only when the real ray from the lamp passes through an
   opening in every floor it crosses, then past the walls of the lamp's room and of the stories in between. The stretch
   inside the receiving story, from where the ray enters it through the opening to the point, is also walked on that
   story's wall grid, so a lamp over an atrium does not light a room of the story below through full walls.
   Borrowed lamps retain wall tests even when a native batch omits its own test; the receiving segment is then tested
   by Apex as well. An unreadable room rejects the borrowed contribution, including adjacent stories. Lamp and sample
   coordinates are transformed once per ray and reused at every floor boundary.
5. **Detail and ambient.** Rooms seen through an opening get the camera story's lighting detail. Rooms joined by removed
   floors into an atrium share one ambient colour and normalisation.
6. **Seamless walls.** Wall samples are lit at the height the wall mesh draws them, and atrium walls are blurred across
   the floor line as one wall.
7. **Updates.** Lamp changes, floor edits, wall or roof changes, lot rebuilds and world loads send exactly the affected
   rooms to gather again, without loops and without re-solving rooms whose lamps did not change.
8. **Outdoor light through openings** (2.13.0). A roofed room with a door, window or arch to the outside also takes the
   outdoor lamps of room 0 of its own story that see one of those openings. Every point of the room is lit by such a lamp
   only through the openings' holes, past every wall of the story and under the room's own roof; the room's own lamps
   keep the game's tests. A room with no known opening takes none.
9. **Indoor light through openings** (2.13.0). A lit lamp of a roofed room shines through its arches and windows (doors
   while doors do not block light) onto the outside walls, the outdoor floors and the lot ground of its story (and of the
   story below, for a house on a foundation), onto its roofless yards, onto the objects and fences outside, and into a
   roofed room next door through an opening between them. Each point is lit only through the opening's hole and past every
   wall of the story; one opening only.

Stairwells, stairs and arches have their own pages: [Light through stairwells](stairwells.md) (the lamp's own walls,
soft slab edges, bounce light and partial adaptation), [Stairs on the directional maps](stairs.md) and
[Light through arches and doorways](arches.md) (arch ambient, floor-level sills, normalisation settling, soft cone
edges and steady indoor light).

### Outdoor lighting queue recovery

Before each native room update, an active, lit outdoor room 0 waiting for a gather regains its missing pending entry when its key is absent from both the changed and pending sets. The native countdown, light collection and complete solve remain unchanged. This prevents thumbnail generation from waiting forever after leaving Build or Buy mode. It runs with the existing story-light update hook and has no separate setting.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Outdoor light between floors | `luzExternaEntreAndares` | bool | on | | Shares outdoor lamps between stories (parts 1 to 3). Installs or removes the whole module live; the other three switches need it |
| Indoor light between floors | `luzInternaEntreAndares` | bool | on | | Indoor lamps light other stories through openings (part 4). A change sends every lot's rooms near openings to gather again |
| Walls block light on floors | `paredesBloqueiamLuzNosPisos` | bool | on | | Decks and yards without a roof stay dark behind a wall: the outdoor floor texels of stories >= 1 store in their map alpha the share of room 0's lamp light that walls block (normal wrapped, 2D and cross-story wall tests); the floor atlas shader takes atlas x (1 - alpha). Steam bytes at 0x006A333B. A change relights every room |
| Light through doors and windows | `luzRealistaPorAberturas` | bool | on (`kRealisticOpeningsDefault`) | | A walled roofless room (a yard, a light well, a deck behind a half wall) also takes the outdoor lamps of its own story that can reach it. Each is tested per point against every wall of the story (`StoryGrid`, from the 1 m lighting tiles): through the doors, windows and arches noted from the game's wall cutout posts (0x00B0D866 / 0x00B0DD15; default sill and head per kind), over walls lower than the lamp (drawn tops of walls taller than a story), and under no roof; the game's own 2D wall test of those lamps is skipped (`GameWallTest`). A yard closed all round takes none. Snow on floors takes the walls' mask too. Needs *Walls block light on floors*. A change relights every room |
| Rooms at Night > Outdoor light through openings | `luzDeForaPelasAberturas` | bool | on | | A roofed room (id > 0, `+0x18` clear) with an opening noted on its outline whose other side is room 0 or a roofless room takes the lamps of room 0 of its story that see one of those openings (part 8, *Outdoor light through openings into roofed rooms*). Each is tested per point against every wall of the story through the openings' holes and under the room's roof, with the game's own wall test of those lamps skipped; the room's objects take them too. A room with no known opening takes none. It also covers a roofed room on a foundation taking the garden lamps of the story below (Developer > Tester sub-part *Roofed rooms take lamps from the story below*, on). A change relights every room |
| Rooms at Night > Doors block light | `portasBloqueiamLuz` | bool | on | | Doors and gates count as closed for lamp light, as the game keeps them closed except while their open animation plays; a cut of unknown kind counts as a door. Arches and windows (light through the glass) stay open. Every reader of the openings follows it: the story grids of the yards and the stories in between, the roofed rooms' doors (a roofed room whose only openings are doors takes no outdoor lamp), the outside walls' copies (objects, fences, windows, the ground shade, the object rigs) and room 0's wall test (*Openings of walls*, `PlaceGridOpenings`). Disabled while neither openings option is on. A change sends again only the stories with such a cut noted |
| Rooms at Night > Indoor light through openings | `luzInternaPelasAberturas` | bool | on | | A lit lamp of a roofed room (id > 0, `+0x18` clear) lights room 0 and the roofless rooms of its story, and a roofed neighbour, through an opening on their outline into its room (part 9, *Indoor light through openings*): tested per point against every wall of the story through the opening's hole, the game's own wall test of it skipped; room 0 of the story below takes those shining out onto room 0 of the story above (a house on a foundation). The outdoor objects and fences take them as per-pixel lamps; a roofed neighbour's objects through their rigs. One opening only. A change relights every room |
| Rooms at Night > Indoor light adds to the ground | `luzInternaSomaNoChao` | bool | on | | That light on story 0's ground adds to the outdoor light instead of the brighter of the two, and the walls' shade of the outdoor lamps leaves it whole ([Indoor light on the ground](#indoor-light-on-the-ground)); needs *Indoor light through openings* and *Walls block light on floors*. The lot pass and the snowy lot passes take it; the outdoor floors do not. A change relights every room |
| Objects > Buildings block lamp light on trees and objects | `paredesBloqueiamLuzNosObjetos` | bool | on | | Outdoor objects, windows and fences drop a per-pixel lamp when an outside wall of the lot, or a placed floor of an upper story above a lamp below them, stands between the lamp and them (a lamp seen past a slab's edge is dimmed), and their ground light is scaled by the share of the nearby lamps' light that is not blocked (`LampVisibility`, see *Walls block light outdoors*); trees, bushes and rig objects behind a building take no garden lamp (Tester sub-part *Buildings block light on trees and objects*). Was *Stories > Buildings block light on objects* |
| Ground > Walls shade lamp light near the ground | `paredesSombreiamOChao` | bool | on | | The lot's ground lamp shade (grass, paths, decks, curbs, stairs and low fences behind an outside wall; soft edges at wall ends and tops) from the same outside-wall copy (`GroundShadeOn`). Tester sub-parts *Walls shade ground and curbs* and *Soft ground shade* |
| Rooms at Night > Shadow softness | `suavidadeDasSombras` | float | 0.15 | 0.02 .. 0.5 | A lamp's radius in metres (the game keeps no bulb or shade size): the penumbra of every edge the walls and slabs give lamp light (*Penumbra*). 0.02 keeps the 4 cm rim of earlier builds next to a perpendicular wall. Applied once the slider is let go; a change sends room 0, the roofless rooms and the roofed rooms with openings of every loaded lot (`SetShadowSoftness`), the object rigs and the ground shade, not every room |
| Game fixes > Seamless walls between floors | `paredesSemEmendaEntreAndares` | bool | on | | Wall samples at their drawn height and atrium walls blurred across the floor line (part 5). A change relights every room |
| Game fixes > Every floor in full detail | `todosOsAndaresEmDetalhe` | bool | on | | Every room of the played lot gets the top lighting detail on every story, so changing floors keeps the light. More solve work when entering a lot. A change relights every room |

All are applied live (`LevelLightShare::Install`/`Uninstall`, `SetIndoor`, `SetWallAlign`, `SetAllFloors`,
`SetObjectWalls`, `SetRealisticOpenings`, `SetOutdoorThroughOpenings`, `SetDoorsBlockLight`, `SetIndoorThroughOpenings`) and reset to their defaults (all on). There is no strength: borrowed lamps keep the game's weight. The module is installed only while Night
Lighting is on. The *Lighting > Stories* card also hosts *Every-Story Ground Light* (`SplitLevelGroundLight`), a separate
patch.

## Compatibility and interactions

- **Walls and floors** ([walls.md](walls.md), [floors.md](floors.md)) read the room maps this module changes; the
  outside wall gain multiplies the result.
- **Objects** ([objects-and-rigs.md](objects-and-rigs.md)): rigs already take outdoor lamps of every story (room id 0
  matches everywhere), so walls and windows now agree on the lamp list. A rig still uses the 3 strongest lamps at the
  object centre without wall shadow; the map sums every lamp per point with wall occlusion. The game's rig gather takes
  only lamps of the object's own room (`light+8 == rig+0x1E0` at `0x6BB333` and `0x6BB283`), so an indoor lamp added to
  another room's list would never reach that room's objects (an atrium's upper window frame stayed black over the
  lower frame lit by the sconce below). ObjectLightBridge therefore runs the game's gather once more for a room-mode rig with the list's lamps of other rooms that reach
  its centre (`LevelLightShare::CrossLampReach`: through an opening, past the walls; share at least 0.25, within 15 m),
  `rig+0x1E0` set to their room id for that call, and gathers those rigs again after every lamp edit (the game marks a
  room-mode rig only for lamps of its own room, `0x006B9230`). Status: "indoor objects given lamps of another story".
- **Indoor objects** drawn by Apex's indoor-object shader read the room's four directional basis maps, which this module
  also guards (see *Directional maps*).
- **Rooms at Night** ([unlit-rooms.md](unlit-rooms.md)): a room whose own lamps are off but which takes lamps through an
  opening is lit by them; the atrium ambient merge works on the colour Rooms at Night produced, and slider changes move
  whole atrium groups together.
- **Sims3SettingsSetter Split-Level Lighting Fix** (`BaseLight::GetLotID` forced to 0 at `0x006BC020`): type-11 lot
  lights then also enter every story through the world-cell part of the gather. Compatible.
- **Sims3SettingsSetter Lighting Quality** (detour of `LightPointWithAllLights` at `0x0069FD60`, original called N times
  on jittered samples): no byte overlap (Apex redirects the call sites, and the return address `0x0069FE19` still
  matches inside the re-entered body), but the cross-story wall tests then run N times.
- **Refresh the lighting** (Lighting > Buildings, shortcut) and the automatic refresh after any lighting setting relight
  every room of every loaded lot, basements included.
- **Daytime curve** (2.12.3, [day-bloom-fixes.md](day-bloom-fixes.md)): in the solves of a roofed room (id > 0,
  `room+0x18` = 0), an outdoor lamp (a street lamp, type 11, or a lot lamp of room 0 with flag `0x04`) takes
  `SolveLampDay(night level)`: 0.03 by day, 1 at night, `LampDay(level)` at twilight, in the wrapped light evaluation
  (`RoofedDay`, before every other test) and in the directional maps (`BasisLightHook`; at night the game's own
  accumulation runs unchanged). Indoor lamps are untouched. Each roofed room an outdoor lamp lit is noted with the factor
  of its solve; once the night level has stayed at day or at night for 2 s, the rooms noted with another factor are sent
  to gather again once (`SweepDayRooms`, log `Daytime curve: N roofed rooms ... gather again`). Status: *daytime curve
  in roofed rooms*.

## Limitations

- Final release-build gameplay confirmation is pending; offline checks do not establish identical material appearance or frame-time cost.

- Daytime curve in roofed rooms: their maps are baked, so between the endpoints they keep the factor of their last solve
  (a lamp switched at dusk solves at its twilight factor); a dawn or dusk sends the noted rooms to gather again once.
  Unverified in game: that the roofed rooms' map texels are all solved through the wrapped evaluation on the light tree
  thread, and the cost of that one-time gather at dawn on large lots.

- Synchronized generation updates do not guarantee instantaneous map calculation; the native solve still has a cost.

- A balcony slab blocks only outdoor light going up (a lamp below it): light going down from a lamp on a balcony is not
  tested against slabs, as occluders are per story. The same holds for type-11 lights under the Split-Level fix. Slab
  planes use each story's lowest floor, so split levels may be off.
- The cross-story wall test runs only on the light tree thread. Points the game solves on another thread get no
  cross-story occlusion (status *on another thread: N*).
- Basements (stories -4 to -1) keep the game's lighting: no sharing, no cascade. Stories above 7 do not exist in the
  tracker.
- Indoor sharing reaches only rooms and lamps within 8 m of an opening, and at most 64 borrowed lamps per room, nearest
  stories first.
- The game tests only the solving room's own walls, and softly; the hard test of the receiving story's other walls is
  Apex's veto (part 4). For a lamp below the room, a wall of the room may block the part of the ray that is still under the floor. A lamp above, the common case, is exact.
- The horizontal sample offset of wall texels is left as the game has it, so a very slight step can remain at wall
  corners. Where a real floor strip separates two walls, the step at the floor line stays by design.
- After a load or a lamp switch, rooms are solved one at a time by the game's queue, so a room can show its previous
  light for a moment. The menu notes that changing floors forces the shown rooms to update.
- The game's other basis-map readers (stairs, instanced objects drawn by game shaders) read the directional maps as
  the game builds them, without Apex's per-object cap.
- Outdoor light through openings reaches a roofed room only through an opening on its own outline to room 0 or a
  roofless room: a room behind another roofed room (light through two doors in a row) takes none. Only room 0's lamps of
  the room's own story and of the story below go in (not those of roofless rooms, whose daytime curve in a roofed room's
  solve is not applied, nor those of stories further down or above). A lamp whose bulb stands under the room's floor
  plane (a post as tall as the foundation) lights the columns and walls inside, not the floor (the game weighs each lamp
  by the texel's N.L). Doors and arches take default shapes per kind, not the game's masks; windows take their panes'
  rectangle (unverified in game) and pass 0.765 of the light. With *Doors block light* on a door
  is closed whether the Sim is walking through it or not (the open animation is not followed) and a glass door lets no
  light through; off, a door is an open hole whether the door object is open or closed (the cut stays while the door
  stands). A cut noted in an earlier load of the same lot (a save loaded again without that door) stays noted: the notes are not cleared when a save is loaded again.
- Indoor light through openings goes through one opening only: a lamp two rooms away (through two arches in a row), or
  light leaving through one opening and coming back in through another, is not taken. Only room 0 of the story below takes
  the story above's lamps shining out (not its roofless rooms, not further stories). The lot ground and the story-0 outdoor
  floors carry these lamps in story 0's room-0 map. With *Indoor light adds to the ground* the lot ground and snowy lots
  add that light after the ground lamp shade; the story-0 outdoor floors still take it in `max(map, atlas)` times the
  shade (next to an outdoor lamp the shade hides, the light from a window is dimmed as much) ; the texels under a story-0 floor tile keep alpha 0, so
  patios and paths keep the look of before. Outdoor draws of room 0's maps take the draws' daytime factor (3% by day) for these lamps too. A lamp added to a
  room's list counts in the game's normalisation of that room (its strongest lamp), as the other borrowed lamps do; a room
  lit only by borrowed lamps gets no boost. The objects outside take such a lamp only below its story's floor where the
  slab copy is known (*Buildings block lamp light on trees and objects*). Cost not measured in game: each lit texel of room 0 within a
  lamp's range walks the grid once per such lamp.
- Outdoor walls drawn below their light base (gables, a garage's eave band beside a house on a foundation, sloped
  foundation faces) are lit from their drawn foot, 5 cm to 3.2 m below the base ([walls.md](walls.md)); unverified in
  game, as are lamps that now test those lower samples.
- Steam 1.67.2 only. The optional parts (detail boost, ambient merge, normalisation guard, directional-map guard,
  seamless walls) each install only when their bytes match and otherwise leave the game's behaviour.

## Technical reference

Outdoor queue recovery checks the outdoor room key in both native pending sets rather than requiring both sets to
be empty. Other rooms may remain pending. Corrupt or unreadable membership prevents recovery, and successful native
insertion is required before counting a repair. Native state, countdown and solve completion are unchanged.
The CPU lamp law uses the existing exact scalar SSE square root. Wall blur buffers are fully overwritten before use,
so repeated zero filling is unnecessary. Neither change reduces samples or lighting detail.


TS3W.exe Steam 1.67.2, image base `0x00400000`. Every site is validated before writing. A mismatch in the core sites
makes `Install` fail with *Light between stories differs (different game version?)*. If only the per-point part fails,
parts 1 and 2 still install and the log says *Per-point evaluation differs; no shadow from the walls of other stories*.
The log line on success is `[LevelLightShare] Installed (walls of the light's story: N of 9 classes; indoor lamps
through stair openings: ...; seamless walls between floors: ...)`.

### Data layout

| Structure | Offset | Meaning |
|---|---|---|
| lot tracker | `+0x6A0 + L*0x1A4` | tree level L (levels -4..7), `TreeLevel(tracker, L)` |
| lot tracker | `+0x90`, `+0xC0/+0xC4` | lot id parts |
| tree level | `+0x00` | room manager of that story (null = story absent) |
| tree level | `+0x04` | back pointer to the tracker |
| tree level | `+0x08` | set of rooms marked changed (buckets `+0x0C`, count `+0x10`, node {id, next}) |
| tree level | `+0x28` | set of rooms pending restart (room ids) |
| tree level | `+0x90` | hash of lights registered on the story: buckets `+0x98`, count `+0x9C`; node `+8` -> {begin, end} of entries, next `+0x10`; entry `+0x1C` = room id, `+0x24` = light, `+0x20` -> record `+0x90` flags |
| tree level | `+0x1A0` | level number (`FUN_006c70c0`) |
| room | `+0x00` | its manager; the manager's `+0x88` = the room's real story |
| room | `+0x0C` | room id (0 = outside) |
| room | `+0x10` | 3D blob occluders (`FUN_006c6a20` + `FUN_0069e5e0`), per story |
| room | `+0x18` | roofless / outdoor classification byte |
| room | `+0x30/+0x34` | 2D occluders (walls) of the room's own story, built by `FUN_006a1de0` from `room+0xD8..+0xDC` (LightingWall + 4) in the room rebuild `FUN_006a2740` |
| room | `+0xC8/+0xCC` | light list (vector of light pointers) |
| room | `+0xF0`, `+0x168` | state of the budgeted solve (1-3 = waiting or being solved, 5 = at rest) / countdown (`+0xF0 == 1 && +0x168 != 0`: already waiting for its gather) |
| room | `+0xF4` | lighting LOD class of the current solve; `== 2` gates the class-2 wall blur |
| room | `+0xF8` | pointer to the room's world matrix |
| room | `+0x110`, `+0x120` | ambient colour (walls, floors) and object ambient |
| room | `+0x160` | light map normalisation |
| room | `+0x584 + i*0x28`, `+0x629`, `+0x62A` | basis map locks, their gate, the basis pass flag |
| room | `+0x639` | wall mode byte read by `FUN_0069fc40` (wall height test / soft shadows) |
| room | `+0x63C` | per-light threshold: the game drops a light whose r+g+b is under it (`0x0069FE40`) |
| room | `+0x640`, `+0x660` | wall-ramp samplers (`FUN_006ab110`: `[0]` base, `[4..7]` colour copy; `+0x650` the colour copy of `+0x640`) |
| room manager | `+0x98` | `+0xD4` plus the story's lowest tile height (`FUN_006a59a0`; 100000 without tiles) |
| room manager | `+0xE0/+0xF0/+0x100/+0x110` | world-to-lot rows (inverse lot matrix, the same on every story) |
| room manager | `+0x220` | the story's LightBasisMap0..3 (bound by `FUN_006a7700`) |
| room manager | `+0x260`, `+0x264`, `+0x268` | lighting tiles (pointers), width, height; index `iz*w + ix` (`FUN_006a42d0`) |
| room manager | `+0x284` | the camera's story |
| room manager | `+0x288` | non-zero = active lot (high lighting quality) |
| lighting tile | `+0x78` | floor height in lot space (`FUN_006a9620`, set from `0x00A880C0`) |
| lighting tile | `+0x7C + q*0x14` | room of quadrant q (`FUN_006a9760`) |
| light | `+0x08` | its room id; `+0x10` intensity; `+0x20` fade; `+0x90` entry flags; `+0xB0` type; `+0xE0` lit colour; `+0x100` flags (`& 0x20` lit); `+0x120` head; `+0x130` range; `+0x134` bounds; `+0x170..+0x1A0` cone (types 4, 5) |
| light manager | `+0xD4` | tree of lot trackers: buckets `+0x58`, count `+0x5C`, node `+8` = tracker |
| root `0x011D1860` | `+0x1C0` | light manager |

Quadrants (`FUN_006aa390`) split a tile along its diagonals: with fx, fz the fractions, `|fx-.5| <= |fz-.5|` gives
`fz > .5 ? 2 : 0`, else `fx > .5 ? 1 : 3`. The floor grid uses the same q (`FUN_006a61a0` passes it straight on).

### The game's gather

Room light lists are built on the light tree thread by `FUN_006c7010 -> FUN_006c6ab0(treeLevel, room)` (thiscall,
`ret 4`):

1. `FUN_006c6990(treeLevel, room, 1)` at `0x006C6B08`: the lot lights registered on that story for that room (filter
   `FUN_006c7820`: entry `+0x90 & 2`, lit `+0x100 & 0x20`, bright enough `FUN_006bc520`, type >= 3, `+0x90 & 4` only for
   type 11, and `entry+0x1C == room+0x0C`).
2. Only for room 0 (`cmp [esi+0xC], ebx; jnz` at `0x006C6B0D`) and only when the tree level is level 0
   (`cmp [edi+0x1A0], ebx; jnz` at `0x006C6B16`): `FUN_006c6990(level0, room, 0)` once more at `0x006C6B2D` (argument
   `lea ecx, [eax+0x6A0]`, the tracker's level 0; the call sequence starts with a `push` at `0x006C6B25`). Level-0 outdoor
   lamps therefore count twice in room 0 of level 0 (type 3 twice, type 11 three times with the Split-Level fix).
3. World lights around the lot from the light cells (`FUN_006b66b0`).

Lot load (`FUN_006c54e0`, `0x006C5525`) gathers room 0 of every story through level 0's tree level; later updates
(`FUN_006c7250`) gather it through the room's own story. `FUN_006c7250` already refreshes room 0 of levels 0..7 when room
0 of level 0 changes (`0x006C73B6..0x006C7426`: `FUN_006a6550(mgr, 0)`, `FUN_0069eed0(room, 1, 0)`, insert 0 into
`+0x28`), which suggests the game intended upper stories to see ground lamps.

### Part 1: outdoor sharing

- Both callers of `FUN_006c6ab0` (`0x006C5816` room creation, `0x006C7094` room update) are redirected (`E8` rel32
  rewrite) to `OutdoorGather(treeLevel, room)`, which calls the original, then under `__try` `NoteRoomStructure`,
  `ShareOutdoorLights`, `ShareIndoorLights` and `NoteGatherStamp`.
- `ShareOutdoorLights` acts only on room 0 whose real story `[room[0] + 0x88]` is 0..7, and only when the tree level
  passed is the room's own story or level 0 (the lot-load path). Sanity: `TreeLevel(tracker, level) == treeLevel` and the
  tracker's tree level of the real story holds the same manager.
- For every other existing story 0..7 it calls `FUN_006c6990(tl_other, room, 0)`, twice for story 0 and once otherwise,
  so each lamp has the multiplicity it has on its own story; the ground story's list does not change.
- `RecordRoom` keeps per room pointer `{mgr, tracker, level, cross[]}`, where `cross` lists (light, home story) of every
  outdoor light registered on the other stories (walk of `+0x90`, entries with room id 0), sorted by light pointer.
  `g_rooms` is cleared on a world change or above 8192 rooms. A reused room pointer is detected by comparing the manager
  (`RoomStillSame`). The gathering thread is remembered (`g_gatherThread`).

### Part 2: refresh cascade

`0x006C73AA` holds `39 86 A0 01 00 00 0F 85 7C 00 00 00` (`cmp [esi+0x1A0], eax` / `jnz 0x6C7432`). The Jcc byte at
`0x006C73B1` changes from `0x85` (jnz) to `0x8C` (jl), so a change of room 0 of any level 0..7 refreshes room 0 of all
stories. Levels below 0 still skip.

When the indoor part's changed-set hook is installed, `InstallIndoor` writes `90 E9` over the `0F 8C` at `0x006C73B0`
instead: every room 0 takes the plain path (its own floor only), and `AfterChangedWalk` sends room 0 of the other
floors 0..7 with the same signature rules as indoor rooms (status *outside of a floor marked changed without a change*).
This removes the game's restart of room 0 on every story for changes that are not lamp changes.

### Part 3: walls of the lamp's story

`LightPointWithAllLights` (`0x0069FD60`, thiscall `(room, out, list2D, list3D, flags, sample)`, `ret 0x14`) sums every
light of the room's list and tests each with `FUN_0069fc40 -> FUN_0069d4c0` against the room's 2D occluders
(`room+0x30`), which are only the walls of the room's own story. `FUN_0069aa90` blocks a ray only below the top of a wall
(no base test). Without a cross-story test, a sconce near a corner of the upper story passed above the lower story's
walls and lit the lower side wall around the corner.

1. The 3 calls of `LightPointWithAllLights` (`0x006A1187`, `0x006A126F` in `FUN_006a0f50`; `0x006A3336` in
   `FUN_006a31d0`) go to `SolvePointSingle` / `SolvePointBatch`. They set a context `g_ctx` (the recorded room info if
   the room is a recorded room and still the same manager, `list2D`, `flags`, batch flag, `room+0x639`, `room+0x63C`)
   and call the original. Only on the gather thread; elsewhere `g_otherThread` is counted. Thread checks read the thread
   id from the TEB (`__readfsdword(0x24)`).
2. The light evaluation `vfunc+0x4C` of all 9 light classes (factory `FUN_006ac590`) is wrapped by a vtable slot write,
   only if the slot holds the expected function and `vfunc+0x24` is `0x009691E0` (light position getter,
   `thiscall(light, float out[4])`):

   | Class vtable | Original `+0x4C` |
   |---|---|
   | `0x00FF42A0` | `0x006BDE90` |
   | `0x00FF42F8` (street lamp) | `0x006BE020` |
   | `0x00FF4350` | `0x006BE1C0` |
   | `0x00FF43A8` | `0x006BEFD0` |
   | `0x00FF44C0` | `0x006BFBA0` |
   | `0x00FF4518` | `0x006BFDC0` |
   | `0x00FF4570` (spot, type 4) | `0x006BFFB0` |
   | `0x00FF4408` CircleWindowLight | `0x006BF880` |
   | `0x00FF4468` TubeLight | `0x006BFA70` |

3. `LightEvalHook<I>` calls the original, then acts only if `g_ctx.info` is set and the return address is `0x0069FE19`
   (after `call edx`, bytes `FF D2` at `0x0069FE17`). `CrossFloorShadow` runs `WallPass` when the light is in the room's
   `cross` list, the colour is non-zero and `flags[0]` is set (the game tests 2D walls in this batch): for each story
   from `min(home, roomLevel)` to `max(home, roomLevel)` except the room's own (the game tests that one next), room 0 of
   that story (`FUN_006a6550(mgr, 0)`) is tested with `FUN_0069fc40(room0, indexVec, lightPos, sample, &t)` (thiscall,
   `ret 0x10`), with its `+0x639` byte temporarily set to the solving room's value (restored after, also on an SEH fault,
   through `g_swapAt`). A failed test gives factor 0, a passed one multiplies by the transmission `t`; the colour (4
   floats) is multiplied by the product.
4. **Per-batch wall culling.** For a batch of samples (`FUN_006a31d0`; its 4 callers push the global batch vector
   `0x01158AC8` = {begin, end}, 0x30 bytes per sample, at `0x006A3B03`, `0x006A3687`, `0x006A37CD`, `0x006A3956`, each
   validated as `68 C8 8A 15 01`), the game builds per light the walls whose culling edge crosses the segment from the
   batch centre (mean of the samples, `FUN_0069f1e0`) to the light: `FUN_006a30b0 -> FUN_0069dff0(walls, int-vector* out,
   from, lightPos)` (call at `0x006A311F`, `ret 0xC`). `BatchCentreFor` recomputes the same centre when the batch changes,
   and `CulledWalls` calls `FUN_0069dff0` on `room0+0x30` of the lamp's story, caching the list per (light, story) for
   the batch (up to 256 entries; the output vector is pre-sized to `walls + 1` so the game never reallocates it). Without
   per-light lists (`list2D` null, or not a batch) all walls are tested, as the game does.
5. The room's own lights are never touched.
6. **From where the ray enters the story:** for an outdoor lamp of a lower story that WallPass let through,
   the game's own wall test of the lit story, which follows (`GameWallTest`, CALL at `0x0069FE93`), runs from where the
   ray reaches that story's lowest floor (`OutdoorEntry`), as for indoor rooms (part 4). That test has no wall base and
   its soft mode shades rays passing near a wall's end, so a ground-floor sconce left the upper story's half wall of a
   balcony dark beside the lit wall next to it (in a recording, all 21 half-wall points were blocked by the game and
   passed by WallPass). A ray that comes up through a floor a player placed on that story or on a story in between (a
   deck, a balcony; `PlacedFloorAt`, not the bare `0x40000000` air key or never-built space) is blocked, also in batches
   without the game's wall test; one passing beside or over a slab's edge keeps the share of the lamp's ball (*Shadow softness*, 0.15 m by default:
   the 0.3 m lantern of earlier builds) the edge leaves (`LampVis::SlabGate`, [`features/lamp_visibility.h`](../../../features/lamp_visibility.h)). Each story's plane
   is its lowest floor; the lamp is the game's own head position.
   The soft mode (`+0x639` = 1, class 2) tests each wall's widened segment (`0x0069AA90` with the soft index, wall `+0x40`
   + `0x30`) and shades by where the ray crosses it (`0x0069A8C0`): a ray rising past a wall's end loses light without
   crossing the wall, which still darkened the upper wall on the other side of the balcony's corner. When that test
   shades such a ray, it is asked again in hard mode (`WallTestHard`, `+0x639` = 0 for the call): the plain 2D crossing,
   with no height test (`0x0069AC04` skips it). If the ray crosses no wall there, the shade was only the penumbra and the
   ray passes; otherwise the soft result stays (it alone knows a ray passed over a low wall). Status: *from where they
   enter its story N (outdoors M, past a wall end's penumbra P)*.
7. **Window light channel test.** The window light's evaluation (type 7, `0x006BEFD0`) asks for every sample whether
   the light belongs to channel 1 through its `vfunc+0x1C` (`0x006C1DD0`, shared by the light classes): the lot object
   by id through ObjectById `0x00C62D40`, then the room's entry in that lot's table `+0x364`. Within one sample batch
   on the light tree thread the light, its lot and that table do not change, so `WindowChannelHook` keeps the answer per
   light and channel for the batch (256 slots, keyed by the batch serial); the first call of a batch, any call outside
   a batch sample (`t_inBatchSample`) and any call on another thread ask the game. The slot is written only when it
   holds the same function as the first light class. Tester switch *Window light channel memo*
   (`perf_window_channel_memo`, on).

### Part 4: indoor light through openings

An indoor room's list comes from `FUN_006c6990(treeLevel, room, 1)`, whose filter requires `entry+0x1C == room+0xC`, and
room ids are unique in a lot, so a lamp only lights its own room. `FUN_006c6990` cannot be reused.

**Floor objects.** The world-side level floor object (0x350 bytes, ctor `0x00A88790`, vtable `0x01062680`): `+0x214`
owner lot, `+0x230` world level, byte `+0x234` (1 = the story's own floor, 0 = a layer), `+0x238` a copy of the story's
lighting manager written once at setup (`0x00A89B60..0x00A89B89`; it goes stale), `+0x264` FloorGrid* (null: no floor).
The manager is found from the lot like the game does (`LevelManager`: owner `+0x214` -> `[+0x23C]` lot lighting ->
`0x00ADBCC0(level)`, a deque of story managers, translated as `LotStoryManager`; the world level code does the same at
`0x00A9D0DC`). Two objects name each story: the lot's floor renderer (`0x00AA1710`; callers `0x00AA35F4` / `0x00AA3690`
with byte 1, `0x00AA4171` / `0x00AA4398` with byte 0) makes the story's own floor (world level L, byte 1) and a layer
at world level L+1 with byte 0, lit by story L: its ceiling, with the outline of the floor above and none of its holes.
Only the own floor (`LevelOwnFloor`) says where the story is open, and `LevelFor(mgr)` returns only that object. Floor
objects are recorded at construction (`LevelCtorHook` on the only ctor CALL at `0x00AA179E`, after `new 0x350`) and at
every floor set or remove: the 5 calls that set or remove a floor quadrant (`FUN_00a89dd0` from `0x00AA0ADB`,
`0x00AA0CCC`, `0x00AA0E4A`, `0x00AA0F72`; `FUN_00a893a0` from `0x00AA05C7`; `ecx = [worldLevel+0x100]`) go through the
naked `FloorSetThunk` / `FloorRemoveThunk` (`NoteLevel(ecx)`, then the game's function).

**FloorGrid** (ctor `0x00A89300`): `+0` data, `+0x10` width, `+0x14` height, 40-byte tiles, quadrant key at `+8 + q*8`
(two dwords); never built = `0xFFFFFFF8 / 0xFFFFFFFF`. A removed floor leaves a key with bit `0x40000000` of its low
dword plus other low bits (`RemovedFloorKey`), for example `4001E000` against `0001E00F` with the floor. The bare
`40000000` (along walls and around a story's floor edge, over rooms that keep their ceiling) is not an opening. Floors
players place have keys without that bit. The lighting side cannot tell a floor from an opening: floor and ceiling
batches light every quadrant of a room.

**Gather** (`ShareIndoorLights`, rooms with id > 0 on stories 0..7, after the game's gather):

1. For each other story U, nearest first (distance 1 to 7, above then below), until the room holds 64 borrowed lamps:
   B = max(S, U) is the boundary floor next to the lamp's side. An `OpeningMask` of B (per tile: holds an opening
   quadrant; lies within `kOpeningReach` = 8 m of one) and the room's span are built once per boundary for the whole
   gather. A mask rather than a list, because an atrium can have more than a thousand opening quadrants.
2. The room must have a tile near an opening of B, and for stories further than one floor every intermediate boundary
   must also have an opening near the room (a conservative filter; the point test checks the real ray).
3. Openings are quadrants with a removed floor on story B over an indoor room (id > 0) of story B-1. The landing around a
   stairwell is room 0 on the upper story (railings close no room), and the air outside a house has no indoor room
   under it.
4. Candidates are the registry entries of the rooms of U near an opening whose lamp stands near an opening and within
   30 m of the room's tiles, whose class evaluation is wrapped, that pass the checks of `FUN_006c7820` (`GameTakesLight`)
   and are not yet in the list. No range test: `+0x130` is the range only for some classes, and wall lights (type 7) hold
   0, 0.1, 1 or garbage there. The 64 nearest are added with the game's `FUN_006a2060(room, light)` (AddRef plus
   push_back).
5. The room is recorded in `g_rooms` with `indoor = true` before each lamp goes in, with per lamp its story, its room
   and the floor object of the highest boundary (other boundaries are resolved through the current managers at solve
   time). `NoteDeps` records which rooms take which rooms' lamps (`g_deps`).

**Per point** (`IndoorShadow` from `LightEvalHook`, `IndoorPassImpl`):

1. The lamp head and the sample are converted to lot space. For each boundary between the lamp's story and the room's
   story, `IndoorBoundaryPass` finds the ray's crossing of that floor (the plane is the story's floor height at the tile under
   the point, else under the lamp, else its lowest floor: on a lot mixing a house on a foundation with one without, the
   lowest floor is the foundation house's, 1.5 m under the other's, and a lower room's lamp lit the floor line of the
   closed room above it), then the height of the tile it lands on. The quadrant there must be an opening over an indoor room of the story below, else the lamp
   gives nothing. Points within 2 cm of the floor plane are tested at their own place.
2. The crossings must be strictly ordered along the ray. Wall segments are built from the actual crossings, not from
   nominal story planes: ghost wall rows and split-level tile heights can place a sample on the lamp's side of a boundary
   despite its room's story number. The segment of the story holding the ray's endpoint is kept when it is not the
   receiving story.
3. When the game tests 2D walls in this batch (`flags[0]`), each segment is tested with `FUN_0069fc40` against the room
   of that story found at the segment's midpoint (the lamp's own room for the first segment), from the segment's start
   to its end, with `+0x639` swapped as in part 3. Transmissions multiply (clamped 0..1). The game's own 2D test, which
   follows in the point solve, covers only the solving room's own walls, in soft mode.
   **Receiving-story wall veto** (`indoor_lamp_walls`, beside the lamp-story check in
   [stairwells.md](stairwells.md)): when the game tests 2D walls in this batch, the sample is not a directional-map
   texel and the lamp is on another story, the stretch from where the ray enters the receiving story (its last floor
   crossing) to the point is walked on that story's wall grid with `GridWallHitHard` (every wall at its height; doors,
   windows and arches open). A ray that crosses a wall is blocked (`PassStage::LampStoryGrid`, counted with the lamp-story
   vetoes as "on its story's own grid" in the status text); every other ray keeps the game's answer.
4. Fail closed: an unreadable floor, a stale manager, a missing intermediate room or an ambiguous crossing order blocks
   the lamp. Exception: for adjacent stories in the normal solve, a missing room on the segment keeps the earlier
   behaviour (pass).

**Directional maps.** The story's four directional basis maps (LightBasisMap0..3, 64x64, 1 texel per metre) are filled
by `FUN_006a09f0` (called at `0x006A3C0A` from `FUN_006a3b80`, state 7 of the room solve `FUN_006a3c90`; only indoor
rooms of lots with `mgr+0x288`, `room+0x62A` set at `0x006A1BB0`): one sample per tile centre (lot x+.5, floor height,
z+.5; world through `room+0xF8`) and, for each light of the room's own list, `FUN_0069f280` (stdcall `(pos, light, float
acc[4][4])`, `ret 0xC`; its only call `0x006A0C56` after `mov ecx, edi`; pos = the sample 0.5 m up) adds the light's rig
colour (`vfunc+0x10`, `FUN_006bdb00`) weighted towards the 4 directions (±0.894, 0.447, 0) and (0, 0.447, ±0.894), with no
threshold, wall or floor test. `BasisLightHook` redirects that call (bytes `8D 94 24 C8 00 00 00 52 8B CF` checked).
For a lamp of another story (`FindCross`):

- `IndoorShadow` runs with wall flags on and the basis flag set, so the floor crossings, every segment including the
  receiving story's (the builder has no wall test of its own) and the raised-room veto are tested.
- **Raised-room veto:** the whole ray is also tested against room 0 (the exterior wall collection) of every story from
  the lamp's to the room's. A raised part of an intermediate story can hold walls outside the nominal story interval;
  any failure or non-finite or zero transmission blocks the lamp. This is a veto only; the segment tests own the glass
  and soft transmission.
- Blocked, the lamp adds nothing; partly transmitted, the game's function runs into a temporary accumulator and only
  that lamp's contribution is scaled.

Status: *directional maps: lamps of another story tested N, behind a floor M*. While this hook is installed and the
indoor part is ready (`BasisFloorGuardReady`), the indoor-object shader omits its floor-map cap; see
[objects-and-rigs.md](objects-and-rigs.md).

**Normalisation guard** (`RoomNormHook`). `FUN_006a0230` (thiscall `(room, float brightest[4])`, its only call
`0x006A13B4` in `FUN_006a0f50`) sets `+0x160 = limit / max(the lamps' vfunc+0x30, brightest sample x k)` when that is
under `[0x01158B24]` or over `[0x01158B20]`. With every lamp blocked this is limit / 0 = inf, the ambient becomes NaN and
the clamp makes it 0. The hook turns a non-finite value into 1, and a room whose lamps are all of another story
(`CrossOnly`) gets at most 1 (no boost): light through an opening is not spread over the whole room, and with none
coming the room takes the unlit colour like an empty room. Status: *rooms lit only by lamps of another story given no
boost N (normalisation not finite M)*.

**Lighting detail** (`LodChoiceHook`). `FUN_0069e710(room)` (fastcall, plain `ret`) picks a room's lighting LOD class:
the max class `[0x01158B00]` (2; 1 with the low lighting setting, set at `0x006A238A`; read by `mov eax,[0x01158B00]` at
+0x4B) only for rooms of the camera's story (`mgr+0x88 == mgr+0x284`) on the active lot (`mgr+0x288`), 0 below. Class
0 samples walls every 0.75 m vertically and about 0.95 m horizontally, class 2 about every 0.25 m. Its 4 CALLs
(`0x0069E82E`, `0x0069EA86`, `0x0069EF46`, `0x0069F1B3`) go through `LodChoiceHook`, which gives the max class to indoor
rooms below the camera story that take lamps through an opening (bit 1, set in the gather) or whose lamps another story
takes (bit 2, queued once when first seen), and, with *Every floor in full detail*, to every room of the active lot. The
game then raises them itself: `FUN_0069ea70` at the end of a solve steps the class 0 -> 1 -> 2 while below
`FUN_0069e710`, and `FUN_0069e770` gives such solves priority (weights `[0x01158B10]` 10000 / 1000 / 100 by class; 0
when the class is above what `FUN_0069e710` asks). Apex's room queue ([`room_light_queue.cpp`](../../../features/room_light_queue.cpp),
`RoomAmbientPolicy::FloorPriorityFactor`) weights the priority lot's rooms 4000 on every floor with full detail,
otherwise 4000 on the camera story, 2000 below and 1 above; other lots 1.

**One ambient per atrium** (`RoomSolveStartHook`, `MergeStackedAmbient`). State 0 of the budgeted room solve
(`FUN_006a18b0`, its only call `0x006A3D0B` in `FUN_006a3c90`) computes per indoor room:

- the ambient colour `room+0x110` in `FUN_006a0f50` (the wall shader adds `lightmap.a x c4`; the parameter table binds a
  pointer to `room+0x110`, so a change shows at once): the room's lights sampled on every second floor tile and 4 m above
  it, over its area (floor quadrants x 0.25 + wall sizes x 3), through the curve `FUN_006a00a0`, clamped to [0, 0.35];
  `+0x120` (objects) likewise, clamped to 0.5;
- the normalisation `room+0x160` (`FUN_006a0230`: 1/m if m < 1, 3/m if m > 3, m = the strongest light or sample);
- the ambient weight ramp of wall samples `(1 - k) + 2k (y - base)/3` (`FUN_006ab210`), base = the story's lowest floor
  (`mgr+0x98`) in the samplers `room+0x640` and `room+0x660`.

After the original state 0, rooms joined by removed floors into an atrium (`ReadStacked`: breadth first over the stories,
at most 12 rooms; an edge needs 16 shared quadrants and 30% of the smaller room's floor, so a stairwell never joins two
rooms) get the same ambient colour (each room's colour, which carries its normalisation, brought to the group's, then
averaged by floor quadrants; written to `+0x110` and the sampler copy `+0x650`) and the smallest normalisation. The ramp
base of the group's lowest story is used for the wall pass only (`WallPassHook` on the CALL `0x006A3D4C` of
`FUN_006a3a30`, thiscall `(room, int, float)`, `ret 8`, returns al: it swaps `room+0x640` around each call), because the
floor, ceiling and object passes share that sampler.

Group coordination:

- The latest group target per room is kept (`g_ambToQueue`) until a merge confirms convergence. A member whose last
  merge differs (colour, normalisation or wall base) is sent to solve again from the next room update, at most every 3 s
  (`AmbientUpdateDue`); a failed queue attempt retries after that cooldown. Pending ambient work counts as busy for the
  post-load settle.
- Compatible colour-only changes (same normalisation and ramp, `AmbientMapsCompatible`) are applied without a native
  solve: targets are staged for every member, and before the lot's room update the whole group is checked (identity,
  finite target, matching revision, unchanged normalisation and ramp, idle ambient ownership), written in that update and
  checked again. A failed write restores the previous colours and falls back to ordinary reconciliation. The wait is at
  most 1.5 s from the first staging (`GroupWaitExpired`). Publication is limited to 16 members per 50 ms globally; pending
  idle updates resume from a key-based cursor (128 visits, 16 writes per poll). An object-rig refresh is requested at the
  next Present after publication.
- Live slider changes from Rooms at Night (`StageAmbientBaseChange`, `StageUnlitAmbientChange`,
  `ApplyAmbientBaseChanges`) update only the background part of each member's original colour, then recompute cached
  groups with the solver's area and normalisation rule, writing the merged colour and its sampler copy together.
- Caches are forgotten only for removed lots or lots whose story managers changed; other lots keep their groups. Only on
  the light tree thread (the lot impostor's synchronous solve goes through the same CALL).

### Lamps under a story's floor

The game lists a ceiling lamp of the story below on the story above, its light a few centimetres under that story's
floor (F8: a ceiling spot 6 cm under story 1's floor lit 3364 points of story 1's closed rooms). It is not another
story's lamp, so it went through none of the tests above. In an indoor room's solve (`OwnFloorBlocksAt`, from
`LightEvalHook`), such a lamp lights a point above the floor only where the ray crosses it through a removed floor
(`FloorAt`; anything unread is a floor). The floor over the lamp is its tile's floor height, the story's lowest floor
only where the lamp has no tile; a point on the floor's plane (the walls' bottom row) is tested where it is. Room 0 keeps
the game's light (lamps on the ground under a raised house). The object rigs drop such a lamp's record the same way
(`LampUnderRoomFloor`, [objects-and-rigs.md](objects-and-rigs.md)).

### Part 5: seamless walls

The wall pass `FUN_006a3a30` lights each piece of each wall (`room+0xD8` list) with `FUN_006ac070(wall, piece, class,
batch)` (`ret 0xC`) -> `FUN_006abdd0`: sample (i, k) at `origin + (i + 0.5) * run / cols + (0, 3k/N, 0)`, texel
`(x0 + i, y0 + N - 1 - k)`, with `cols = wall+class*0x10+0x28`, `N = +0x2C`, the atlas block `{x0, y0, x1, y1}` at
`wall+class*0x20+0x58` (`y1 = y0 + N - 1`), `run = wall+0xF0`, `origin = wall+0x110`, and 3.0 the float at `0x00FF37DC`.
Rows cover [0, 3): the top row is 3/N under the top (N = 13 / 7 / 4 for classes 2 / 1 / 0: 0.23 / 0.43 / 0.75 m). The wall
meshes take their light UVs from `FUN_006ac200` (through `FUN_006a5600`, called by the straight wall builder
`FUN_00c38530` and `CurvedWallGeometry::UpdateLightingTexCoords` `FUN_00a5e0a0`): `v = (y0 + 0.5)/H` at the top vertices,
`(y1 + 0.5)/H` at the bottom, linear between, so row k is drawn at `k * 3/(N - 1)`. Every row is drawn higher than where
it was lit: the wall below a floor line shows the light from 3/N under the line, the wall above shows the light at the
line (next to a sconce 1.2 m under the line: 0.86 against 0.46 at class 2).

- `WallSamplesHook` (CALL at `0x006A3AF5`): after the game fills the batch with a piece, each sample moves to
  `k * 3/(N - 1)` (`y += k * (3/(N - 1) - 3/N)`), after checking the whole piece against the formula above (otherwise
  the piece stays as the game has it). Both walls then end on the light at the line.
- The class-2 blur `FUN_0069f650` (fastcall `(room)`, CALL at `0x006A3B62`; `[0x01158B1C]` = 2 passes, mode
  `[0x011D02E4]` = 0: each pass `[1 2 1]` along every row of the block, then down every column, clamped to the block;
  `cmp [room+0xF4],2` at +0x0D) would pull the edge rows towards the inside of each wall (top row ->
  `(10 e0 + 5 e1 + e2)/16`). `WallBlurHook` keeps the top and bottom rows out of the vertical blur (blurred along the row
  only, with the game's rounding `(a | b) - ((a ^ b) >> 1 & 0x7F7F7F7F)`).
- For rooms of an atrium group (`GhostRoom`: members of `g_wallBase`, both lit at class 2), `WallSolveHook` (CALL of
  `FUN_006a31d0` at `0x006A3B0A`; thiscall `(room, batch {begin, end}, atlas {base, pitch}, char flags[2], sampler, char
  ambient)`, `ret 0x14`, writes each texel at `Y * pitch + X * 4`) also lights copies of the top-row samples 1 and 2 rows
  (0.25 m) higher and of the bottom-row samples 1 and 2 rows lower into a private 4 x n buffer (`g_ghosts`, per wall,
  reset at piece 0). `BlurWalls` then blurs every class-2 block with the game's algorithm with those rows around it and
  writes it back, so the walls below and above blur across the line as one wall. Room 0 is left out: outside walls of
  stories under the camera are lit at class 0 (no blur), so the wall above must end on the exact light at the line.
- The horizontal offset (samples at `(i + 0.5) * run/cols`, drawn texel centre to texel centre) is left alone: samples on
  a wall's very end sit on the next wall's line, where the 2D wall test is ambiguous.

The three wall-pass redirects install all together or not at all. Status: *seamless walls between floors: on (N wall
samples moved to their drawn height, N wall pieces left as the game has them, N walls blurred across their edges, N edge
rows kept out of the blur)*.

### Walls block light outdoors

Outdoor floors take `max(floor map, ground atlas)`, outdoor objects take per-pixel lamps and the atlas, and the atlas
carries room 0's lamps with no walls, so a wall lamp outside would light a yard without a roof, an upper deck behind its
half wall and the objects in them.

- **Floors** (`paredesBloqueiamLuzNosPisos`, `SetFloorWalls`; Steam 1.67.2 fixed addresses only): for outdoor floor
  texels of stories 1 and up, the batch solve evaluates room 0's lamps at the texel with the normal wrapped (only walls
  decide), with and without the 2D and cross-story wall tests, and stores the blocked share in the texel's alpha (0 for
  every outdoor texel in the game). A lower story's lamp more than 2 cm under the texel is left out of both evaluations
  (the texel's own solve weighs it by N.L; on floors more than 2 m over the lot base the atlas share of such lamps is
  removed separately). 0x006A333B (`cmp byte [ebx+18h],0; xorps xmm1,xmm1`, the load of `out[3]`) becomes a
  call that supplies it. The floor atlas shader takes `atlas x (1 - alpha)`. A change relights every room.
  Story 0 is not masked here (`MaskedFloorRoom0` returns nothing below story 1): its room 0 also lights the outdoor floors
  placed on the ground, with no wall test, but its map is the lot pass's lot map too. Those floors are shaded in their
  shaders by the lot's ground lamp shade instead ([floors.md](floors.md)).
- **Objects** (`paredesBloqueiamLuzNosObjetos`, `SetObjectWalls`, `WallBlocks`): room 0's outside walls of each story
  (the LightingWall list: centre line from `+0x110` and `+0xF0` behind the `+0x150` normal, 3 m high) are copied when
  that room is solved, on the light tree thread (`g_wallSnap`, per story manager). The render thread tests a
  lamp-to-object segment against a 4 m grid of the copy, at the height the ray crosses each wall. A blocked lamp leaves
  the object's per-pixel list (the next one takes the slot), and the object's ground light is scaled by the share of the
  nearby lamps' light that is not blocked. Fences use the same test from their group's centre and both ends
  ([fences.md](fences.md)). Slabs count too (`LampVisibility`): each story's placed floors (4 bits per tile) are copied
  with room 0's walls (`CaptureStoryFloors`, `g_floorSnapGen`), and a lamp below the tested point loses what the floors
  of every loaded lot stop (`LampVis::SlabGate`, the room solve's rule): left out at 0, its colour scaled by the share
  left at a slab's edge (fences keep the largest share of their three points).
  The same copy also serves the object rigs' gather from any thread (`LampVisibilityShared`: an immutable
  copy published by `OnPresent` when the walls or slabs change, [objects-and-rigs.md](objects-and-rigs.md)) and the lamp shade
  of lot ground (`OutsideWallsIn`, [lot-light-pass.md](lot-light-pass.md)). Since 2.13.0 each copied wall carries its
  doors, windows, arches and half-wall cells (*Openings in the outside walls' copies* below).

### Openings of walls (light through doors and windows)

- **Capture** (installed with the module whatever the option, Steam bytes only): `Lot::AddWallAndFenceCutouts`
  `0x00B0D460` and its removal `0x00B0D920` post message `0x06257EB4` for each wall cell an object cuts. The push of the
  id at `0x00B0D866` / `0x00B0DD15` becomes a call of `CutAddThunk` / `CutRemoveThunk`, which note the payload (lot id
  `+0x08/+0x0C`, edge key `+0x10` {x0, z0, story, x1, z1, story}, mask `+0x18`, all zero = closed again) with the
  product's kind (`+0xD0`: door, window, gate, arch).
- **Lot id:** a cut is filed under the post's own lot id (`g_cuts`, per lot id and story). That id is the posting lot's
  `+0x48/+0x4C` (`0x00B0D792`), the one the game routes the message by (`0x00C52AA0`). A post with no lot id, with an
  unreadable payload or with a key that is not one side edge of a cell is rejected. The table outlives switching Night
  Lights off and on (`UninstallCutoutCapture` keeps it; only a new world's lots replace it): the game posts a lot's cuts only
  when it loads, so a cleared table closed every arch and door to light until the save was loaded again. A cut made while
  Night Lights is off is missed until its lot loads again. The lot's story manager
  (`lot+0x23C` -> `0x00ADBCC0`) is read apart from the payload, for the notes only.
- **Lookup:** a story's grid asks for the cuts under one of two ids (`CutLotFor`): the id of the lot that owns its story
  manager (the story's own floor object, `LevelFor`, its owner lot `+0x214`, that lot's `+0x48/+0x4C`) or the manager's
  own `+0x90/+0x94`, whichever some post carried (the owner lot's when both did). With neither the story has no opening
  and its walls stay closed. In game the owner lot's id differed from the posts' on several lots (the owner lot
  `3647CD2033346670` of the posts' `B1280029950F0CA0`), and the owner lot alone found no opening on any of their stories.
- **Grids:** `StoryGrid` holds every wall of a story from its 1 m lighting tiles, with each noted cut placed on its edge
  (`PlaceGridOpenings`, `OpeningDefaults`: sill and head per kind; the mask shapes are not read yet). Roofless rooms and the stories lit from
  below build theirs as before. Since 2.13.0 room 0's gather of any story, 0 included, also builds the grid of a story
  with openings noted (`RoomZeroOpenGrids`, with either openings option), and the stories whose openings change are built at the next gather
  (`DrainWarmStories`). These grids stay in their own cache (`g_warmGrids`): the floor mask's visibility test
  (`OutdoorFloorVisibility`) reads only the grids of the room paths; these serve the openings (roofed rooms, the outside walls' copies, room 0's wall test). A later request of
  the same story takes the grid over without building it again.
- **Doors block light** (`portasBloqueiamLuz`, 2.13.0, default on): `PlaceGridOpenings` counts each cut on a wall by kind
  (`StoryGrid::kinds`) and leaves a door, a gate or a cut of unknown kind closed (`CutOpensForLamps`): the wall stays
  whole there for every reader of the grid. A window or an arch keeps its hole; a window beside a closed door keeps its
  jamb on that side. The kinds come from the product's `+0xD0` (`CutKind`: `0x100` arch, `0x2` door, `0x8` gate, `0x4`
  window); in the game logs of several lots every post had one of `00000002`, `80000002`, `00000004` or `00000100`, none of
  unknown kind. The state is part of a grid's stamp (`StoryGrid::doorsBlock`) and of the outside walls' copy
  (`StoryWallCopy::doorsBlock`). A change (`SetDoorsBlockLight`) gives every story with such a cut a new cut generation
  and sends it as a story whose openings changed (`RequeueCutStories`: its roofless rooms and its room 0; then
  `DrainWarmStories`: its grid, its walls' copy, the roofed rooms whose doors changed); at most 512 stories, no other
  room. The game opens a door only during its animation; a door seen open at that moment, and glass doors, are not
  modelled: real-time door state belongs to per-pixel lighting on the GPU, and a glass door would need the cutout mask's
  shape (no resource read path yet).
- **Closes held** ([`cut_close_hold.h`](../../../features/cut_close_hold.h)): a cut closed again counts for lamp light
  only after it stays closed for 2 s (`CutCloseHold::kHoldMs`, applied by `RequeueCutStories`); an add of the same edge
  within that time cancels it. In game, camera turns made the game post a lot's cuts again (21 posts each, 3 times): its
  window cuts closed and opened again 0.18 to 0.67 s later. Each close took the windows out of the story's grid and a
  roofed room behind them gathered with 2 doors to the outside, then with 5, and its light depended on how the camera had
  moved (`[Openings] ... windows 3 -> 0` then `0 -> 3` in the log, a kitchen dark from one side and lit from the other).
  The `[Openings]` line and the *outdoor light through openings* status count the closes reopened within the hold and
  those that lasted. Unverified: which game path re-posts the cuts on a camera move.
- **Windows by the window built:** the hold did not cover the camera: it closes the window cuts of the wall it looks
  through and keeps them closed while it stays (a later capture: `windows 6 -> 3` at a camera turn, 18 closes that lasted,
  the kitchen's `[RoofedLamps]` gathers alternating 8 and 5 doors to the outside, every window light still listed). So a
  window's close no longer goes through the hold nor changes the story's cuts: it marks the cut closed
  (`CutRec::closed`, `ApplyCutPost`), with no new generation. The grid keeps a closed window while a window light (type
  7 or 8) stands in front of its cell or a sized pane lies over it (`WindowLightIn`, `KeepBuiltWindows`: a window three
  cells wide has its lights centred in its middle cell, and a centre test alone left its end cells out at each camera
  turn, `windows 6 -> 4` in a later capture); a deleted window loses its lights and
  is left out (and leaves the lot's geometry key, `StoryCutsContent`). `EnsureStoryGrid` builds the grid again when the
  set of window cuts with no light changes (`UnlitWindows`, at most once a second, as for glass doors), and an add of a
  window left out gives a new generation. Doors, arches and gates keep the hold. A window with no window light of its own
  (none known in the base game) closes and opens with the camera as before, without the hold. The `[Openings]` line
  counts the window closes marked.
- **Grids built too early:** a grid built while its lot streams in can have no wall edge yet (in game: stories 1..4
  of `B1280029950F0CA0` with 79 / 98 / 38 / 8 noted cuts and no wall, and no room of theirs gathered again). A grid with
  no wall edge on a story with noted cuts or roofed tiles is not kept (`NoteGridReadiness`); the rooms on its tiles (at
  most 64) gather again every second, at most 12 times (`RequeueGridRetries`, render thread), and their stories' openings
  are looked at again on the light tree thread. The first grid with walls is logged (`[YardGrid] ... rebuilt with N wall
  edges and M openings after K retries`); after 12 tries the grid is kept as it is.
- **Window sizes** (2.15.3): a window takes the glass rectangle of its type 7 daylight panes (`WindowPanes`). The
  RectangleWindowLight constructor `0x006C0870` stores half the width and half the height of its record at
  `+0x210` / `+0x214`, and its daylight `0x006BEBF0` fades where the ray's local x and y along the axes `+0x190` and
  `+0x1B0` pass them, so each pane is a rectangle centred on the light. For each window cut on the grid, the union of the
  panes in front of its cell (near the wall line, overlapping the cell), clipped to the cell, gives its jambs, sill and
  flat head; a shape outside the limits (sill above 2.2 m, head under 0.15 m over it or over the wall's top, under a
  tenth of the cell wide) or a window with no readable pane keeps its kind's defaults. Unverified in game: the values
  read and that the pane's rectangle is the glass and not a larger daylight patch (developer builds log the windows sized
  from their panes).
- **Wall depth** (2.15.3): the game lights a wall from two faces 0.05 m each side of the edge line (`FUN_006AB280`), so a
  wall is 0.1 m thick for light. Where the lamp's spot at either face reaches a cell with an opening, the ray is tested at
  both faces and keeps the smaller share (`WallOpenings::kHalfThick`; the faces' shift capped at 0.5 m), so the beam
  narrows at grazing angles (each jamb takes 9 cm more at 60 degrees off the normal), in the grid walk and in the outside
  walls' copy. The mesh's own thickness is not read.
- **Glass** (2.15.3): a window, and a door open only for its glass, passes `WallOpenings::kWindowPass` = 0.765 of the
  light (glass 0.85 x frame and bars 0.9, a chosen constant: the game keeps no glass value for lamp light); arches and
  open doors pass all of it.
- **Shapes:** doors and arches take their kind's sill, head (5 heights across it, flat) and jambs (`OpeningDefaults`). A
  cache of shapes per cutout mask (`MaskShapeFor`, the post's TGI and piece) and the scan of a mask's alpha
  (`WallOpenings::ShapeFromAlpha`: span, sill and the head's curve) are in place, but nothing fills the cache: no path in
  the codebase reads a resource's bytes (the resource cache only answers which package holds a key), and the record and
  stream layouts such a read needs are not verified, so doors and arches keep their kind's defaults.
- **Sends:** the stories whose openings changed send their roofless rooms once their posts are quiet for 250 ms
  (`RequeueCutStories`), matching each loaded lot by its managers' id, else by its owner lot's id. With *Outdoor light
  through openings* the same stories go to the light tree thread (`DrainWarmStories` -> `RequeueRoofedDoors`).
- **Jambs:** an opening's side with no jamb (`u0 = 0` / `u1 = 1`: the next cell of a wide door, window or arch is cut
  too) has no soft rim since 2.13.0 (`GridEdgePass`); the rim there left a half-dark line where a ray crossed the seam
  between two cells of one opening (found by the offline check `tools/roofed_openings_test`).

### Openings in the outside walls' copies

Since 2.13.0, with either *Light through doors and windows* or *Outdoor light through openings* active. The copy of room 0's
outside walls (`CaptureStoryWalls`, *Walls block light outdoors* above) treated every wall as solid from its base to 3 m,
so a lamp lit nothing past a door, window or arch: a garden post inside an arcade left the grass, the columns and the
fences outside dark except where a ray slipped past the end of a wall (hard lit rectangles in front of the arcade).

- **Patches** (`StoreStoryWalls` -> `FillWallPatches`, light tree thread): each copied wall is laid on its story grid
  (its line `x = X` or `z = Z` in lot space, within 15 cm; a diagonal wall keeps none). Per 1 m cell with a grid wall, a
  patch (`WallOpenings::Patch`, [`wall_openings.h`](../../../features/wall_openings.h)) where the grid's top of that cell
  is lower than the copy's (a half wall: its base + 1 m; a drawn top under the copy's) or the cell holds an opening (its
  span between the jambs, sill and head, from the edge's base, in world y). A cell with no grid wall, a story with no
  grid or no wall read yet, and a diagonal wall keep the copy's whole wall (fail closed). The patches are taken again
  when the walls, the story's cuts (`CopyStoryCuts` generation), the drawn tops (`g_wallTopGen`) or the option change, a
  story with no grid tries again every 2 s, and a story whose openings changed takes them from the light tree thread
  (`DrainWarmStories` -> `RefreshStoryWallPatches`); any change moves `g_wallSnapGen`.
- **Test** (`WallOpenings::CrossPass`): a ray crossing a wall between its base and top passes over a patch's own top, and
  through its opening with the grid walk's penumbra (*Penumbra*: the lamp's spot at the crossing, spread over the patches
  beside, so two cut cells side by side leave no seam; not past the copy's ends). Elsewhere it is stopped as before.
- **Users:** objects, fences and windows on the render thread (`WallPass` in `LampVisibility`; the share is multiplied
  into the slab gate's), the object rigs' copy (`LampVisibilityShared`, the published `VisSnap` carries the patches; a
  wall listed in several 4 m cells is counted once, in the cell of its crossing point), and the ground lamp shade
  (`OutsideWallsIn` hands the patches to `GroundShadow::ShadeJob`, which keeps a share per texel instead of a blocked
  bit, [lot-light-pass.md](lot-light-pass.md)). The copy's patches skip the sill rim of an arch or door on the floor,
  as the grid walk does (2.15.3: the ground just past an arch lost up to 10% of an outdoor lamp to that rim).
- **Room 0's own wall test** (`GameWallTest` -> `OpenWallRetest`): room 0 of a story with openings on its walls keeps its
  story's grid and the one above (`RoomZeroOpenGrids`, at its gather). When the game's test shades a ray (blocked or
  partly) the ray is walked on those grids (`GridPassImpl`: every wall at its height, the roofed quadrants); only when
  the walk passed through an opening does its share replace the game's, if larger. The walls behind an arch take a
  lamp's light as the ground and objects do.
- **Diagnostics:** the status line *outside walls' openings* (stories copied, with a grid, cells with an opening or a
  lower top, render and rig tests through an opening, room 0's shaded rays walked and lit); the lot pass status counts
  the ground shade's tests through a wall's patches; developer mode logs `[WallCopy] lot L story S: N outside walls
  copied ..., C cells with an opening and H lower than the copy` when a story's copy changes; F8 lists the stories, the
  first 40 walls with an opening (span, sill, head, jambs) and the last 24 lamp tests through an opening (lamp, point,
  share, wall, where it crossed); the Full capture prints every copied wall with its patches.

### Outdoor light through openings into roofed rooms

Option `luzDeForaPelasAberturas` (`SetOutdoorThroughOpenings`, 2.13.0). Active (`RoofedOpeningsOn`) when on, installed,
with the room update hooks, `GameWallTest`, the wrapped light evaluation and the cutout capture (`g_cutReady`; without
it no opening is known and nothing is taken). It does not need *Walls block light on floors*: a roofed room has no
floor mask.

- **Gather** (`ShareRoofedOpenings`, from `OutdoorGather` after the indoor and roofless shares, light tree thread): for a
  roofed room (id > 0, `+0x18` clear) of stories 0..7 whose story has cuts noted, its story's grid (and the story
  above's) is taken from the openings cache (`EnsureStoryGrid(mgr, warm = true)`: `g_grids` stays the yards' and the
  floor mask's). Its doors (`RoofedDoors`) are the edges of its outline with an opening whose other side (the next tile's
  quadrant) is room 0 or a roofless room; with none, nothing is taken and its record and copies go. The story's registry
  is walked as `GatherSameStory` walks it, room 0's entries only: not a window light (types 7, 8), a wrapped class
  (`EvalWrapped`), an outdoor lamp (`OutdoorLamp`: type 11, or `+0x100 & 0x04` with room `+0x08 == 0`, so the daytime
  curve applies to it in the room's solve), not already in the list, not a lamp of another story the indoor share took;
  within its range (`LampReach`, `+0x130`, else 16 m) of the outline's box and of a door; and seeing a door's hole
  (`DoorSeen`: 3 x 3 points between the jambs, from sill to head, on the edge line, each ray through `GridPassImpl` on the
  story's grid and the story above's). Then the game's own checks (`GameTakesLight`); a lamp that sees a door but is off
  is noted for the sends, not taken. At most 64 lamps. The room's record (`RoomInfo`, `openings = true`, the lamps in
  `same`, the grids) is written before the first lamp goes in with the game's adder `FUN_006a2060`, story 0's room-0
  lamps twice (their weight there, as for the yards). The indoor share's record of the same gather is kept (`indoor`
  stays true: the room's own lamps and the lamps of other stories keep their tests).
- **The story below's lamps** (2.13.0): a house on a foundation has its ground floor on story 1 and its garden posts on
  story 0 (in game, lot `B1280029950F0CA0`: the colonnade rooms of story 1 found 13 to 16 doors each and
  considered no lamp). A roofed room of story S >= 1 also walks room 0's entries of story S - 1, with the same checks;
  `DoorSeen` then also tests the walls of story S - 1 (its grid from the openings cache; none readable: no such lamp is
  taken). Those lamps are listed in `RoomInfo::lower` with that grid (`below`).
- **Per point** (`LightEvalApex`): `RoofedDay` first (the solve's `SolveLampDay` factor, as for every outdoor lamp of a
  roofed room); on wall texels with *Realistic lamp reach* the outside walls' distance law (`LampReach::Share`), so a wall
  just inside the arch and the outside wall next to it fall off alike; then `YardShadow` -> `YardPass`: the walls of the
  story at the ray's height, through the openings' holes (jambs, sill, head), the roofed quadrants' ceiling (floor + 3 m;
  a ray over a wall's top meets the room's own roof), the story above for a ray that high. The game's own wall test of
  that lamp is skipped (`GameWallTest`). A lamp of the story below (`OpenLampPass`) also passes the walls of its story
  and the floors of the room's story (`OutdoorEntry`'s slab gate: a lamp under a deck or a floor of the room's story stays
  out). Directional maps (`BasisLightHook`): the same test at the basis sample, the contribution scaled by it (0: left
  out), then the day factor. Normalisation (`RoomNormHook`): a room whose list holds
  only borrowed lamps (other stories' or these) gets no boost.
- **At the outdoor scale** ([`opening_lamp_balance.h`](../../../features/opening_lamp_balance.h)): outside, the same lamp
  is drawn from room 0's map, cut at 1 per channel, times the outdoor surface's brightness (the lot pass: *Ground
  brightness* x *Lot lamp brightness*; outside walls: the walls' *Brightness*), and room 0 is not normalised. Inside, the
  room solve summed it whole, with no cut before the room's curve, and the room's normalisation multiplied it. A light
  capture of a green sconce (intensity 1.7, colour (0, 1, 0), a story-0 room-0 lamp listed twice) beside a courtyard: the
  kitchen behind its glass door showed 0.808 on screen, the courtyard at the same distance at most 0.567 of its map.
  Each entry of such a lamp is now held under 1 / its entries per channel (the outdoor cut), then takes the opening's
  share and glass, then the outdoor scale: floors, ceilings and the directional maps the ground's (`Ground brightness x
  Lot lamp brightness` while *Street lamps light lots* is on, else 1), wall texels the walls' (*Lamps light walls* on,
  else 1), over the room's normalisation (`InnerLight::NormScale(1, room+0x160)`; the directional maps take no
  normalisation factor, as whether the game normalises them is not read). Offline (`tools/roofed_openings_test`) the
  kitchen pixel replayed through the room solve's curve and the room-lit floor shader's (PS `44ECECA0`, `c2.z` 4.2) was
  brighter than the courtyard at every one of 200 distances before and is at none from an entry of 0.05 up after. A slider
  change relights every room once the slider is let go (`SetOpeningLampBalance`). Indoor lamps, the lamps of other
  stories, the roofless rooms and daylight are unchanged. Unverified: the indoor wall shaders' curve (taken to match the
  floor's), so the walls' share of this balance is inferred.
- **Walls ending at a floor above** (2.13.0, `CapLowerWalls`): a story's grid caps a full straight wall with a placed
  floor of the story above beside it, over its foot and lower than a story, at that floor (the rule of `PassesOverLowerWall`
  and `CapFoundationSides`). A foundation's sides are walls of story 0 drawn only up to the floor on the foundation; at 3 m
  they hid every point of a colonnade on the foundation from a post on the terrain (in game: the floor, the columns
  and the back wall dark). The grids are built again after a floor edit (`g_floorEditGen`) and once the lot's stories are
  known. The story above's grid is asked only above its own floor (`StoryGrid::plane`, `GridEdgePass`'s floor): a wall of
  that story drawn down past its floor stands in the story below, whose grid holds its walls. The `[RoofedLamps]` line
  names the story above's grid and the capped walls of the story below's, and for a lamp hidden from every door which grid
  hides its nearest door.
- **Objects:** each room's lamps and its grids are published as an immutable copy (`PublishOpenRig`, a shared pointer
  under a short lock) that the object rigs read on any thread (`OpeningLampReach`, [objects-and-rigs.md](objects-and-rigs.md#cpu-fixes-objectlightbridge)).
  A lamp of the story below also passes that story's grid and the slab copy of the room's story (`FloorSnap`,
  `LampVis::SlabGate`); with no slab copy yet the objects take none of it.
- **Sends:** a room with doors is noted (`g_roofedRecs`: its outline's box, its doors' signature `DoorSig`, the lamps
  that saw a door). A change of room 0's lamps of its story sends it (`SameStoryRoofed`, from `AfterChangedWalk`, let
  through with the yards' sends): when the change is known lamp by lamp (`LampChange`'s exact pairs), only the rooms one
  of the changed lamps saw (and, for a move or a player's edit, those it can reach now); else (a lamp added or removed, a
  wall changed) the rooms that took one or that one can reach. The roofed rooms of the story above are sent the same way
  for a change of room 0's lamps. A lamp switch's safety net (`RelightLampSwitch`) sends
  every noted room of the lamp's lot for an outdoor lamp. The cutout posts send, once quiet, the roofed rooms beside an
  opening whose doors are not the ones their last gather saw (`RequeueRoofedDoors`; the room whose gather is running is
  left alone).
- **Diagnostics (doors):** each `[YardGrid]` line counts the story's cuts on walls by kind (doors, windows, arches,
  gates, unknown) and those closed for lamps, with the rule's state; F8 (*openings of walls*) prints the rule, how often
  it was switched and the stories sent, the closed cuts per lot and story, and per loaded story the grid's kinds, closed
  cuts and the state it was built with. The F7 notes of a lamp through an opening are unchanged: a closed door makes no
  opening, so a lamp behind it is not listed.
- **Diagnostics:** the log line `[RoofedLamps] Room solve: story S room R: N outdoor lights through its openings: L... (D
  doors to the outside, C room 0 lamps considered (of them of the story below, and taken), B beyond reach, H with no door
  in sight)` when a room's count or
  doors change (developer mode also logs a room with no door to the outside); the status *outdoor light through
  openings* (rooms, lamps, points tested and lit, directional-map samples, sends, copies published); F8 lists every room
  of the copy with its lamps; the F7 wall block marks such a lamp with its share at the pixel.

### Indoor light through openings

Option `luzInternaPelasAberturas` (`SetIndoorThroughOpenings`, 2.13.0). Active (`InnerOpeningsOn`) with the same parts as
*Outdoor light through openings*: installed, the room update hooks, `GameWallTest`, the wrapped light evaluation and the
cutout capture.

- **Gather** (`ShareInnerLamps`, last in `OutdoorGather`, light tree thread): for any room of stories 0..7 whose story has
  cuts noted, its openings into roofed rooms are the edges of its outline with an opening the grid keeps open whose other
  side is a roofed room (id > 0, not roofless, not itself; `InnerDoors`, at most 256). The story's registry is walked for
  the entries of those rooms: not window lights (types 7, 8), not street lamps (11), not outdoor lamps (`OutdoorLamp`), a
  wrapped class, not in the list (`ListHolds`), not taken by another share (`cross`, `same`); within their range of one
  of their room's openings (`LampReach`); seeing its hole from the lamp's side (`DoorSeen`). Then the game's checks
  (`GameTakesLight`), at most 64, added once with the game's adder (its weight in its own room). Room 0 also walks the
  openings of story S + 1 onto its room 0 and those lamps (`innerUp`). The record is written before the first lamp goes
  in (`RoomInfo::inner`, `innerUp`, the grids); a fresh record of a roofed or roofless room is an indoor one, so its own
  lamps keep the game's tests.
- **Per point** (`InnerShadow` -> `InnerPass`): `YardPass` on the story's grid and the one above (asked only above its
  floor); a lamp of the story above also passes its own story's grid, goes down through that story's plane only outside
  its roofed quadrants and off any placed floor (`InnerDownGate`, the live floor grid), then the walls of this story. The
  game's wall test of the lamp is skipped (`GameWallTest`). In the floor mask's copies of room 0 these lamps give 0
  (the ground atlas holds none of them; the room's own map carries them). Realistic reach applies on room 0's and the
  roofless rooms' wall texels as for any lamp there; not in a roofed room. No daytime curve (`RoofedDay` is for outdoor
  lamps). Directional maps (`BasisLightHook`): the same test at the basis sample. Normalisation (`RoomNormHook`): these
  lamps count as borrowed (`CrossOnly`).
- **Objects:** a roofed room's lamps of its neighbours are published with its rig copy (`OpenRigRoom::inner`); the rigs
  take them through the game's per-light test with `rig+0x1E0` set to the lamp's room for the call (`RigLightTestAsRoom`,
  their share from `OpeningLampReach`), as the room-list gather's wall test would pop them. The lamps room 0 and the
  roofless rooms take are published for the render thread (`ExitLamps`, `ExitLampVisibility`): outdoor objects and fences
  pick them with the outdoor lamps (`SelectLamps(..., true)`; not windows, roofs, water or glass), tested on the lamp's
  story grid, below its floor through the roofed quadrants and the slab copy, then the outside walls' copy
  (`LampVisibility`); they stay out of an object's ground share (the atlas holds none of them).
- **Sends:** each room's openings into roofed rooms are noted (`g_innerRecs`: their signature, the rooms behind them, those
  whose lamps it took). A change of a source room's lamps sends the rooms with an opening into it, a value change only
  those that took one (`SameStoryInner`, `AfterChangedWalk`), and room 0 of the story below for the story above's rooms.
  A change of a story's openings sends the rooms whose openings differ from their last gather and room 0 of the story
  below (`RequeueInnerDoors`, from `DrainWarmStories`).
- **Brightness on both sides of an opening:** the game multiplies an indoor room's summed light by its normalisation
  (`room+0x160`, [room-light-maps](../../engine/room-light-maps.md)) and an outdoor room's by nothing, so a lamp's light
  through a window onto a balcony was its raw sum while the room around the lamp showed it x that factor (in game,
  F7 of the balcony floor: 0.20 in its map 0.45 m outside the window, about half the room's own floor at the same distance).
  In a texel's own solve (`SolvePoint`) the lamp's colour is scaled by the source room's factor over the solving room's,
  1 for room 0 and the roofless rooms (`InnerNormScale`, `InnerLight::NormScale` in
  [`inner_retry.h`](../../../features/inner_retry.h)). The strip right outside a window stays dark where the ray passes
  under its sill: a window without a readable pane keeps its kind's default sill (0.85 m).
- **A lifted lamp:** while a player drags a lamp the game takes it out of its room's registry until it is put down, and a
  neighbour that gathers in between takes nothing from that room; once put down, only the rooms holding the lamp were
  sent, so the neighbour stayed dark (in game). A gather that misses a source room's lamps it took before notes them
  (`InnerRec::lost`, `NoteInnerLost`) and gathers again after 0.7, 1.5, 3 and 6 s while it keeps missing them, four times
  at most ([`inner_retry.h`](../../../features/inner_retry.h), `SendInnerRetries` on the render thread); a value change of
  that room sends it too (`SameStoryInner`), and a lamp edit's switch or move sends every room with an opening into the
  lamp's room that holds none of the lamps (`RelightLampSwitch`, `RelightLot`). Log lines: `[InnerLamps] Story S room R:
  the lamps of room X are gone at this gather (a lamp lifted?): it gathers again in N ms`, `[InnerLamps] Story S room R
  gathers again: it missed the lamps of N rooms at its last gather (try T of 4)` and `[InnerLamps] <why>: N rooms with an
  opening into the lamp's room gather again`.
- **Diagnostics:** the log line `[InnerLamps] Room solve: story S room R: N indoor lights through its openings: L... (room
  X through (x,z)-(x,z)) (D openings into K roofed rooms; C lamps considered, B beyond reach, H with no opening in sight)`
  when a room's count or openings change; the status *indoor light through openings*; F8 lists the rooms and the lamps
  shining out; the F7 wall block marks a lamp of a roofed neighbour with its share at the pixel.

### Indoor light on the ground

Option `luzInternaSomaNoChao` (`SetIndoorLightOnGround`). Active with *Indoor light through
openings*, *Walls block light on floors* and the floor mask's alpha site. Story 0's room 0 is the lot pass's lot map and
the floor map of story 0's outdoor floors, which their draws combine with the ground atlas as `max(map, atlas) x M` (M the
outdoor lamps' wall shade): a window's light on the grass under it was lost in the max next to a street lamp's pool and
darkened by M.

- **Solve** (`SolveGroundTexel`): each floor texel of that room in the budgeted solve's floor pass (`room+0xEC == 3`,
  `FUN_006a3c90` on `+0x180`) outside a floor tile keeps in its alpha (0 for room 0 in the game, written through the floor mask's site
  0x006A333B) `E = min(Lum3(exit), 254/255)`, the light of the lamps taken through openings (`innerLamp`). The game's
  running sum is noted before each light (`LightEvalHook`, as the outdoor delta does) and an exit lamp's part is what the
  sum gained by the next light or the end, so the drop under the room's live threshold, the soft cut's fade and the 3D
  test count as in the texel. Alpha 1 stays a roofless room's own floor. The synchronous solve (impostors, `0x006A3EC0`)
  is not recognised (unverified whether it moves `+0xEC`): its texels keep alpha 0.
- **Draws:** in the lot pass and the snowy lot passes, which draw story 0 only, the share `s = sat(a / lum(map))` of
  the map leaves the max and the shade and is added after them
  ([lot-light-pass](lot-light-pass.md#indoor-light-on-the-ground), [snow](snow.md)). The outdoor floors keep the plain
  patch: a floor draw cannot tell story 0's map from a higher story's, whose alpha is the walls' mask
  ([floors](floors.md#indoor-light-on-the-ground)).
- **Quick outdoor lamp edits:** a delta whose changed light is such a lamp solves room 0 in full (the kept parts hold no
  alpha; `LampRec::exit`, `ExitLampOf`).
- Developer status: *Indoor light on the ground* (ground texels tapped, lit through openings).

### Outdoor texels keep their hue

The game packs a room texel as `min(sum, 1)` channel by channel. On an outdoor texel (room 0, a roofless room) where a
strong lamp meets a weaker light of another colour, the strong lamp's channel stopped at 1 while the weaker light filled the
others. F7 of a red sconce (intensity 1.7, listed twice) on a courtyard's decorative wall, beside a kitchen's glass doors and
windows: the lot map by the lamp held (1, 1, 0.41), the courtyard ground and the lattice wall showed the yellow light
baked in room 0's map there (not a GPU lamp's: the red lamp adds no green) instead of red, while the kitchen behind the glass (the lamp at the outdoor cut, *Outdoor light through
openings*) was red. The lot pass, its shade and the indoor light on the ground were not the cause: the shade there is 1,
the ground atlas holds the lamp's red, and the texels' alpha E is at most 3/255 (the exit share `ex` at most 1.3 % of the
map).

`SolvePointBatch` now divides an outdoor texel's sum by its largest channel when that passes 1 (`OutdoorDeltaModel::KeepHue`)
before the game's pack, after the kept parts are noted (Quick outdoor lamp edits keep the whole sum). The deltas
(`PackInput`) and Real-time outdoor lamps' compose shader pack the same way, so the three paths still give the same texel. A
sum within 1 is packed as before, bit for bit. In the courtyard's case (red 5.8 + yellow (1, 1, 0.41)) the texel is
(255, 38, 15) instead of (255, 255, 105). A single lamp past 1 keeps its colour too: the hottest spot of a warm lamp on an
outside wall is its warm colour at full brightness instead of near white (on the grass the ground atlas keeps the white
spot through the lot pass's max). Indoor rooms keep the game's pack. Checked offline: `tools/outdoor_delta_test` (the
pack within 1 bit for bit, ratios past 1, the courtyard's case) and `tools/rt_lamps_test` (the compose shader against the
pack on texels past 1). Unverified in game.

### Penumbra

Every edge the walls and slabs give lamp light is soft by the lamp's size (2.13.0,
[`features/penumbra.h`](../../../features/penumbra.h)). A lamp is a ball of radius R (*Shadow softness*); a point sees it
through the cone from the point to the ball, which where the ray from the lamp (t = 0) to the point (t = 1) crosses an
edge is a disc of half width R (1 - t) across the ray (similar triangles): sharp right behind a jamb, wider farther
behind it. On a vertical wall the disc stretches by the ray's slant (`Penumbra::SlantOf`: 1 / cos of the horizontal angle
along the wall, sqrt(1 - (d.T)^2) / |d.N| up it), at most 1 m. The share of the disc on the open side of an edge is
`smoothstep(x / 2h + 1/2)` (`Penumbra::Cover`): the cumulative of a parabolic kernel, so the weight of a stretch [a, b] is
`Cover(c - a) - Cover(c - b)` and stretches side by side add up exactly. Nothing is sampled: one evaluation per crossing.

- **Grid walk** (`GridPassImpl`, light tree thread and the object rigs' copy): at each wall line crossed, the spot is
  spread along the line (`GridLinePass`) over the cell crossed and its neighbours where the line goes on: both walled (an
  opening's next cell, the next piece of a wall), both open, or a wall's free end (`GridVertexFree`: no other wall or
  diagonal at the vertex). Each cell's stretch passes by its pieces (`GridEdgeSpot`): solid over its top, the opening
  `Cover(sill) x Cover(head)` or over its top, an empty cell or one under its foot whole. At an L or T joint the spot is not
  spread, so a closed corner leaks nothing. The rule that a side whose next cell is cut too has no rim follows by itself.
  Diagonal walls keep the spot within their edge. MidPass uses the same penumbra (it had a fixed 0.3 m rim).
- **Light maps:** while a room solve evaluates its map's texels (`SolvePoint`) the half width is also at least 0.75 of the
  map's texel at the point (`t_mapAA` = 0.75 / texels per tile of the room's class, 0x00FF36AC; `Penumbra::Half` takes
  `aa t` at the crossing): an edge covers at least 1.5 texels, so a sharp edge behind a jamb no longer draws as texel
  stairs (4 texels a metre at class 2, 8 with *Light detail* high). The maps keep the game's filtering.
- **Outside walls' copies** (`WallOpenings::CrossPass`, `SegPass`, the ground shade): the same spot over the patches; the
  wall between patches stops its share; past a copy's end the spot is not spread.
- **Slabs** (`LampVis::SlabGate`): the band of the same radius (it was a 0.3 m lantern), at least 4 cm.
- **Minimum:** half width 2 cm (`Penumbra::kMinHalf`, the 4 cm rim of earlier builds); at R = 0.02 m and a ray square to
  the wall the edges are the earlier ones exactly (`tools/roofed_openings_test`).

### Updates

`FUN_006c5e20` pushes `FUN_006c7250` (per-story room update, fastcall `(treeLevel)`, plain `ret`) as a function pointer
(`push 0x6C7250` at `0x006C5E2A`) that `FUN_006c4b40` calls for levels -4..7 of every lot. The immediate is replaced by
`RoomUpdateHook` (`BeforeRoomUpdate` first). Its call that empties the changed-rooms set (`call 0x7F3790` at
`0x006C7497`, `ecx = tl+8`, thiscall `(set, buckets, count)`, `ret 8`) goes through `ChangedClearHook`.

- **Changed rooms.** The set is read just before it is emptied (it is filled inside the update itself: a dirty lamp
  entry's vfunc+8 calls `FUN_006c7160`). The rooms of other stories that take those rooms' lamps (`g_deps`) are sent
  again, never the story being updated.
- **Signatures** (`RoomLampSignature`, `LampChange`). The game marks a room changed for more than lamp changes (the
  entry update `0x006C7BA0` does it for any lit lamp whose entry is updated). A changed room sends its takers only when
  its lamps or walls differ from the last send. Two hashes: the shape (which lamps, cone, the room's walls `room+0x30`)
  and the values (lit flag, lit colour `+0xE0`, intensity `+0x10`, cone `+0x170..+0x1A0` for types 4 and 5). Position
  and range are tracked per lamp at the last send (`LampAt`): a move counts beyond 10 cm, a range change beyond 2%. A new
  shape sends the takers at once and stops their solve; a value change sends them at once only if they were not sent for
  a value change in the last 1 s, without stopping a solve; later value changes wait for 300 ms of quiet, at most 1.5 s
  (`kDepQuiet`, `kDepRecent`, `kDepMaxWait`; `g_depWait`, `FlushDepWaits`).
- **Lot state** (`LotState`, every 2 s per lot on level 0): the lot's 8 story managers are compared (a rebuilt lot starts
  again: *lots rebuilt N*); when the number of stories with a known floor object grows or the opening count changes, or
  `g_indoorGen` moved (install, option, `OnWorldLive`), or the lot's floors were edited, the rooms near its openings (both
  sides) and the rooms holding lamps of another story gather again (`QueueOpeningRooms`).
- **Settle after loading.** A lot that finds its openings sends those rooms once more when its first round is over: none
  of its watched rooms gathers, waits or solves (states 1-3), none is held and no lamp burst waits, for 400 ms, not before
  500 ms after arming and at the latest after 6 s (`kSettleMin`, `kSettleQuiet`, `kSettleMax`).
- **Floor edits.** Floor set or remove calls mark the floor object; after 250 ms without another edit
  (`RoomAmbientPolicy::FloorEditReady`) the room list is marked stale, Rooms at Night is told, and the lot's rooms near
  openings gather again.
- **Structure changes.** `NoteRoomStructure` hashes each indoor room's manager, id, roofless byte `+0x18` and wall list
  (`room+0x30`, at most 1024 entries) at every gather. A changed signature queues that room; at most every 250 ms
  (`StructureRefreshDue`), Present marks the cached room list stale and calls `UnlitRooms::OnRoomChanged` for each queued
  room. Lamp colour, animation and lighting LOD are not part of the signature. The cache holds at most 8192 rooms and is
  cleared on a world change.
- **Window lights.** `FUN_006c7ba0` (light registry entry vfunc+8) resolves the room at the window sample, reads its
  roofless flag (`FUN_006c7b20`, `FUN_0069e620`), checks its paired entry and the tree's state, passes the computed lit
  boolean to `FUN_006bdca0` (which alone only sets the flag and colour) and marks rooms with `FUN_006c7160`. On a new lot
  or manager, a world-live generation, a geometry refresh or a change of the displayed story, `BeforeRoomUpdate` runs this
  evaluator for the lot's window lights (type 7/8, validated evaluator at vfunc+8, registry snapshotted first) at once,
  after 2 s and after 6 s (`WindowRecheckDue`). The game decides whether each window is lit. `LightEntryUpdate` has two
  signatures, both resolving to `0x006C7BA0`. The evaluator marks the window's room changed whatever it found, so a
  recheck that changed nothing had every window room of the lot solved again at each story change and lamp edit: its room
  marks (through `LampMarkFilter`'s mark call) are kept back (`BeginDeferMarks`) and made only when the entry's room, lit
  bit or lit colour (`+0xE0`) changed, else dropped (the log line counts them). A fault inside the evaluator lets them
  through.
- **Stale switched-off lamps.** The game switches a lamp off through its intensity (`+0x10 = 0`, so `+0xE0 = 0`) and
  keeps the lit flag. `OnPresent` scans once a second every indoor room at rest (state not 1..3) of the loaded lots
  (`ForEachRoomImpl` in lazy mode: tile walk at most every 10 s, lots gone skipped) and sends a room whose list still
  holds a lamp the gather filter now refuses (`GameTakesLight`: flag 0x20 and `FUN_006bc520`, the sum of `+0xE0..+0xE8`
  >= `[0x010459E4]`), once per set of such lamps (`g_staleSent`, kept while the room is busy). Room 0, window lights
  (types 7 and 8) and street lamps (type 11) are excluded.
- **Queueing** (`QueueRoom`) is the game's own refresh: `FUN_006a6550(mgr, id)` (thiscall, `ret 4`), `FUN_0069eed0(room,
  1, 0)` (invalidate, `ret 8`), insert into `tl+0x28` with `0x00B7AAD0` (thiscall `(set, out, const int* key, char)`,
  `ret 0xC`), skipped when already pending. Rooms are never put into the changed set, so a queued room never sends
  others. A value-change send defers while the room is being solved.
- **Room enumeration** (`ForEachRoom`, `CachedStoryRooms`) is rebuilt at most every 3 s, or at once when the lot list,
  the story managers or a room structure changed.
- **Install / uninstall** (`RefreshAllLots`, render thread): walks the lot tracker tree (`lightMgr+0xD4`) and queues room
  0 of levels 0..7 exactly like `FUN_006c7250`. Uninstall also sends the rooms in `g_indoorList` (rooms holding lamps of
  another story; kept across world-change clears of `g_rooms`), so no lamp keeps shining through a floor without the test.
- Option changes of other Night Lighting settings leave this module installed (`ReinstallNow`'s `reinstalling` flag).

### Floor switches (lamp mark filter)

`features/lamp_mark_filter.cpp`. On every floor switch the game restarts room 0 of every story and every room holding a
lamp (`0x006C7451` in `FUN_006c7250`). The lamp entry update `FUN_006c7ba0` (entry vtable `0x00FF5984` slot +8; for a lamp
light vfunc+0x18 = `0x00620D60` returns false) finds the lamp's room (`FUN_006c7b20`), rewrites its lit bit and colour
(`FUN_006bdca0`) and marks the room changed (`FUN_006c7160`, thiscall `(tl, room)`, `ret 4`: `0x006C7CCA` for the room
left, `0x006C7CD6` always) without comparing. Entries are flagged by `FUN_006c4cf0` from the light manager's messages
(transform `0x3361F6C9` at `0x006B0A8D` and colour `0x966EC80A` at `0x006B0BFA` always; intensity `0x006B0B33`, enable
`0x006B0C9A`, alpha `0x006B0D26` only on a change). Nothing in the room solve reads the shown story or a light's
visibility.

- `MarkThunk` on the call at `0x006C7CD6` (esi = entry, edi = light, ecx = tl) hashes the lamp's values (its room, lit bit,
  object flags `entry+0x20` record `+0x90 & 6`, `+0x10` x4, type `+0xB0`, `+0xC0..+0xDC`, `+0xE0..+0xF0`, position
  `+0x120` x3, range `+0x130`, cone `+0x170` x13 for types 4 and 5) and compares them with its last mark per (tree level,
  light). The same values drop the mark; first sight, window lights, unreadable lamps and the filter switched off always
  mark. Not filtered: `0x006C7CCA`, occluder entries (`0x006C7939`), room creation and object removal.
- `MarkDecide` keeps per lamp whether it is on (lit bit and colour sum > 1e-3), its position and room. A change makes the
  lot due after 120 ms for a pure on/off change or 200 ms for a move or room change (`LampRefreshDelay`), then
  `LampMarkFilter::OnPresent` runs `LevelLightShare::RelightLot` (every room of the lot, stories -4..7, room 0 too) and the
  rig refresh, at most once per 2 s per lot, or 500 ms for a lamp into another room or story or many lamps
  (`LampLotGap`). Nothing happens while the night level is between 0.02 and 0.98
  (dusk and dawn switch every lamp), without a world, for first sight, for window lights, or for a light switching more
  than 3 times in 10 s.
- A switch-only relight may keep a room that is at rest (state 5), has the same manager and id, gathered strictly after
  the last switch (`NoteGatherStamp`, `GatherAfterChange`; the stamp is written only after the original gather and both
  sharing steps succeed) and has no pending ambient, dependent or deferred work. The stamp cache is bounded and cleared on
  a world change.
- Developer > Lighting has a checkbox for the filter (on, not saved) and its status; the recorder writes *Rooms keep their
  light: ...*.

### After loading

`OnWorldLive` (called when Night Lighting sees the world drawn) bumps `g_indoorGen`, so every lot's rooms near openings
gather once more and settle. `RequestRefreshAfterLoad` then runs `NightLighting::RefreshAll("after loading")` (lots,
every room, rigs; the terrain keeps the load's own rebuild) once the room list is fresh and no room is busy or pending
for 250 ms, polled every 200 ms from 500 ms after world-live, and at the latest after 8 s (`AfterLoadRefreshReady`). The
room enumeration is expired at world-live. A setting change pending at that moment takes over.

### Wall seam recording

Starting a recording (Developer > Recorder) arms `BeginSeamRecording`; saving writes `Wall seams.csv` to the recording
folder, cancelling discards it. Only wall samples (|normal.y| <= 0.3) of indoor rooms that are not roofless, from batch
solves other than ghost rows, within 2.5 cm of the story base or base + 3 m, are kept, in a separate 8,192-entry ring
(latest samples retained). Columns: `elapsed_ms, lot, story, room, class, x, y, z, nx, ny, nz, story_base, raw_r, raw_g,
raw_b, normalization, after_curve_sum`. Raw samples precede the wall blur and atlas upload. Recording does not change
lighting, diagnostic arming, solves or budgets. An empty file means no qualifying wall-edge solve happened.

### Diagnostics (developer mode)

- Status line *Stories* on the Developer page (see the strings in `LevelLightShare::Status`).
- F8 (`light_diag.cpp`, namespace `LightDiag`) appends `DiagText()`: the per-story room 0 lists, the section
  `==== ANDARES (luz externa entre andares) ====` (samples within 3.5 m of each light of the active lot: point story,
  light, type, home story, point, normal, colour sum, the game's wall test (1 passed / 0 blocked / -1 not run) and factor,
  Apex's factor, batch and culled-list flags, per-light per-story summary), `==== STORIES INDOORS ====` (per story: tiles,
  floor grid, every floor object naming it with "its floor" / "the ceiling layer", openings; the indoor gathers),
  `==== SEAM ====` (latest wall solves of atrium rooms with their LOD class) and `==== SOLVES ====` (kept from the world
  load: ambient steps and wall passes of rooms sharing light, with thread, LOD class solving and shown, state, lamps,
  boost and merge flags, normalisation, ambient, ramp base, `Q` sent by Apex, `H` held until the solve ended, lot id,
  camera story and the room flag `+0x19` whose change invalidates a room, `0x006A5E00 -> 0x0069F160`).
- F8 also lists the noted openings: cut cells per post lot id and story (by kind, with the lot object of the last post),
  the lot ids whose story manager carried another id, and for each loaded lot and story where its cuts are looked up
  (the manager's `+0x90` id, the owner lot and its id, how it was found, the cut cells found, its grid if built).
- The log (developer mode) holds the first 10 posts of each lot id, rejected ones included, with the raw key bytes and
  the reason (`[Openings] lot ... post N`), one line per lot id whose manager carried another id (manager, `+0x88`,
  `+0x90`), and one `[YardGrid]` line per lot and story when its grid is built or its walls or openings change, with how
  the lot was found.
- Samples are recorded only while armed (Developer > Lighting *Record story light samples for the diagnostics*, or the
  first F8 of a session). The game's wall test is wrapped (`GameWallTest`, CALL at `0x0069FE93`) in every build (for the
  entry points of parts 3 and 4); the development build also records its result.
- F8 (indoor diagnostics) prints *Window lights' channel test*: kept per batch or not installed, how often the game was
  asked and how often the answer was kept for the sample batch (part 3, step 7).

### Address reference

| Address | What | Check |
|---|---|---|
| `0x006C6AB0` | AddWorldLights `(treeLevel, room)` | call targets checked |
| `0x006C5816`, `0x006C7094` | its two callers | `E8` + rel32 == target, redirected to `OutdoorGather` |
| `0x006C6990` | per-story gather `(treeLevel, room, char ownFloor)`, `ret 8` | calls at `0x006C6B08`, `0x006C6B2D` |
| `0x006A6550` | room by id `thiscall(manager, id)`, `ret 4` | call at `0x006C73F0` |
| `0x0069EED0` | invalidate room `thiscall(room, char full, char keep)`, `ret 8` | call at `0x006C73FF` |
| `0x00B7AAD0` | set insert, `ret 0xC` | call at `0x006C741B` |
| `0x006C73AA` / `0x006C73B1` | cascade cmp/jnz; Jcc byte `0x85 -> 0x8C` (or `90 E9` at `0x006C73B0` with the indoor part) | `ValidateBytes`, `WriteBytes` |
| `0x0069FD60` | `LightPointWithAllLights` | calls at `0x006A1187`, `0x006A126F`, `0x006A3336` |
| `0x0069FE19` | return after `call edx` | `FF D2` at `0x0069FE17` |
| `0x0069FC40` | wall test `thiscall(room, int* idx, lightPos, sample, float* t)`, `ret 0x10` | its call at `0x0069FE93` (developer mode: `GameWallTest`) |
| `0x0069DFF0` | wall culling, `ret 0xC` | call at `0x006A311F` |
| `0x01158AC8` | global batch sample vector | 4 pushes validated |
| `0x009691E0` | light position `vfunc+0x24` | per class |
| `0x011D1860` | root; `+0x1C0` light manager | `RefreshAllLots` |
| `0x006C7820` / `0x006BC520` / `0x006A2060` | gather filter; bright enough `fastcall(light)` (CALL at `0x006C7848`); add light to room `thiscall(room, light)`, `ret 4` (CALL at `0x006C7874`) | signature |
| `0x006C5E2A` / `0x006C7250` | `push imm32` of the room update / the update | byte `0x68` and imm32 checked |
| `0x006C7497` / `0x007F3790` | changed-set clear | `E8` within 0x400 of the update |
| `0x00A89DD0` / `0x00A893A0` | floor set / remove | 4 / 1 callers (`CallersOf`), each `E8` validated, no branch lands inside |
| `0x00AA179E` / `0x00A88790` / `0x01062680` | floor object ctor CALL / ctor / vtable | ctor writes the vtable at +0x0D (`C7 06`) |
| `0x0069E710` / `0x01158B00` | LOD choice / max class | 4 callers; `A1` + address at +0x4B |
| `0x006A3D0B` / `0x006A18B0` | state 0 CALL / function | signature, `E8` |
| `0x006A3D4C` / `0x006A3A30` | wall pass CALL / function | signature, `E8` |
| `0x006A3AF5` / `0x006AC070` | wall piece samples | within 0x150 of the wall pass |
| `0x006A3B62` / `0x0069F650` | class-2 wall blur | within the wall pass; `8B 0D` at +0x20, `80 3D` at +0x88 |
| `0x006A3B0A` / `0x006A31D0` | batch solve per piece | point-solve call within 0x300 |
| `0x01158B1C` / `0x011D02E4` | blur passes (2) / blur mode (0) | `Deref` |
| `0x006A13B4` / `0x006A0230` | normalisation CALL / function | Steam 1.67.2 only |
| `0x006A0C56` / `0x0069F280` | basis light CALL / function | bytes `8D 94 24 C8 00 00 00 52 8B CF` |
| `0x006C7CD6` | lamp mark call (`MarkThunk`) | bytes checked, Steam 1.67.2 only |

### Cost

The lower-wall pass reuses the existing per-point floor lookup.
The last-manager shortcut and its live owner check remain. When successive rays alternate between stories, a floor
already looked up by `OutdoorEntry` or another lamp at that same point need not lock and scan the links again. The
existing point generation expires this cache at the next sample. This changes no ray, wall list, light parameter,
gather delay, solve class, queue order or time budget. The lookup comparison and its limits are recorded in
[validation](../../validation/night-lighting-level-light-share.md#exterior-floor-lookup-2026-10-07).

The point solve runs for every texel of a room light map. The hooks read the thread id from the TEB and do nothing for
rooms without borrowed lamps. Outdoor light through openings adds, per texel of a roofed room that took such lamps, one
grid walk per lamp (about one step per metre between the lamp and the texel, two grids for a ray that reaches the story
above), and per gather of a roofed room of a story with openings a scan of the story's tiles for its outline and 9 rays
per door for each room 0 lamp in reach. A story with no opening noted builds nothing. The wall and slab test of a lamp
and a point for objects is kept while every input it reads is unchanged (same float bits and the same wall, floor and
penumbra state), so a repeated query returns the value it would compute. Caches keyed by a story manager or lot (the
outside walls' and slabs' copies `g_wallSnap` and `g_floorSnap`, the story grid caches) are pruned once their key has
been missing from the loaded lots in two sweeps in a row (`liveness_prune.h`). Diagnostic sample recording exists only in developer mode and runs only while armed. Every
floor in full detail adds solve work once when entering a lot. In-game cost has not been measured (see
[validation](../../validation/night-lighting-level-light-share.md)).

The hot loops of the room solve (`level_light_share.cpp`, `stairwell_bounce.h`, `penumbra.h`, `lamp_visibility.h`) take
square roots and absolute values of floats from `ApexMath::Sqrt` and `ApexMath::Fabs`
([`features/fast_math.h`](../../../features/fast_math.h)): `sqrtss` and a sign-bit mask, inline. Under `/fp:precise`
`std::sqrt(float)` and `std::fabs(float)` are calls to the CRT, which ranked among the hottest Apex samples inside the
game's room solve. `sqrtss` is correctly rounded like `sqrtf` and clearing the sign bit is exactly `fabsf`, so every
result is bit for bit the same; the double overloads keep the standard functions. The light position (`vfunc+0x24`) is
kept per point generation in 64 slots, one per light by its address (a room sees up to 32 lamps); a light that misses
its slot asks the game again, so every position is the game's own. The stairwell timing total (a diagnostic, one
solving thread) is a plain load and store instead of a 64-bit locked add.

## Closed-floor guards, directional weights and the retired mesh blockers

Wall-blur support rows outside a story cannot borrow a lamp through a placed slab merely because the synthetic row
lies on the lamp's side of that slab. A lower support row of an upper wall tests the recipient floor; an upper support
row of a lower wall tests the floor above. Open floor quadrants retain their previous continuity. This supplements
the ordinary ray tests without changing visible sample positions, light colours or filter weights.

Opening-imported directional contributions also take the source-to-recipient normalisation ratio already used by
the floor/wall solve. Gains above one accumulate into a temporary contribution before joining earlier lamps, as do
fractional shares. The receiving room's normalisation is the one its last solve ended with
([arches.md](arches.md)).

**Mesh blockers (retired).** Indoor light through openings and between stories no longer tests triangles of opaque
instanced meshes (stairs, railings) collected from the screen. That set followed the camera (a triangle budget by
on-screen coverage) and its test was hard, so the light changed with the view and every publication relit the rooms
near openings ([arches.md](arches.md), step 6). With the shipped behaviour, the draw hooks read no mesh buffers, the
collected set is dropped once and nothing is published. The collector (`features/mesh_occlusion.h`,
`lot_light_bridge.cpp` `NoteSolidMesh` and `PumpSolidMeshes`) remains for comparison in development builds, where two
refinements apply: an occlusion scene identical to the last published one is not published again, and buffer reads
inside draw hooks never wait for the GPU (a busy buffer is retried, then waited for after three busy answers; the same
rule applies to the fence groups of the lot ground shade).

Source: `level_light_share.cpp` `InnerNormScaleAt`, `BasisLightHook`, `InnerPass` and `IndoorPassImpl`.

## Rejected approaches

- Sharing lamps without a wall test: lower side walls near corners became brighter than the upper ones. Details in
  [history](../../history/night-lighting-level-light-share.md).
- Testing indoor light against opaque meshes collected from the screen: the light followed the camera and every
  publication relit the lots ([history](../../history/night-lighting-level-light-share.md)).
- Replacing lower-coverage geometry when the mesh budget filled: it did not make the collected set complete.
- Only the game's soft 2D wall test for the lamp's own story: lamps leaked through walls that meet the slab
  ([stairwells](stairwells.md)).
- Re-queueing other stories by a light-list signature: could re-enter a story whose pending set was being walked.
- Reading the story from the tree level passed in: the lot-load path gathers every story through level 0.
- Testing another story's walls without swapping the wall-mode byte `+0x639`.
- Pairing a story with its floor through the floor object's `+0x238` copy, or with whichever of the two objects naming the
  story was seen last.
- Treating the bare `40000000` floor key, or only the never-built key, as an opening.
- Requiring one ray crossing per nominal story boundary.
- Clearing every group member's source on each gather and merging only when all sources are present (*Test 002*): more
  repeated solves and inconsistent group colours.
- Rebuilding a world-wide lighting refresh periodically instead of tracking lamp, floor and structure changes.
- A balcony share (*Light behind balconies*) lit the wall behind a slab from a lamp beneath it (2026-10-12, captures
  18-38-41); a placed floor now blocks a lamp below it ([history](../../history/night-lighting-level-light-share.md)).
- Filing a lot's cuts under its story manager's `+0x90/+0x94` id through an alias made by the first post that differed
  (an earlier design): read while a lot streamed in after the world was still being built, it moved every cut of that lot
  under an id its grids never asked for ([history](../../history/night-lighting-level-light-share.md)).
- Taking a roofed room's outdoor lamps into its object rigs through the game's room-list gather `FUN_006bb2f0`
  (`GatherAsRoom` with id 0): it drops the street-lamp class (`vfunc+0x20` true) and its 2D wall test against the room's
  own walls pops any lamp behind the arch's wall, which it sees as a whole run (decompiled 2026-10-13). The per-light
  test `FUN_006bb270` is called alone instead ([objects-and-rigs.md](objects-and-rigs.md)).
- Masking story 0's room-0 solve for floor texels (2026-10-13, F7 08-16-00, a paved path lit through a free-standing
  wall): that map is also the lot pass's lot map, which the pass already multiplies by the lot's ground lamp shade, so the
  grass would be shaded twice. The floor shaders take the shade instead ([floors.md](floors.md)).
- Scaling the light a roofed room takes through its openings to match the room 0 surfaces outside (2026-10-14, F8
  20-43-05: a red post 0.54 m over an arcade's floor, its recess floor saturated, the paving outside much dimmer). The
  room solve gives both sides the same law (room 2's normalisation was 1.000, no boost, and the lamp was an outdoor
  lamp, not *Indoor light through openings*). The step at the arch comes from the 8-bit map clipping the sum at 1 (4 to
  5 within 3 m of that post) and from the surface shaders: interior floors apply the tanh curve (gain about 1.35 to 1.46)
  while outdoor floors and lot grass take `c3.x` x *Ground brightness* (and *Lot lamp brightness*). A factor between them
  would be an artistic share, not physics. Unverified: which map (the GPU relight or the game's solve) the outside showed
  at the capture, since that capture's Instant mode kept dropping the active lot's kept parts (over the 128 MB budget) and
  sending its room 0s back to the game's solve.

## See also

- [Validation](../../validation/night-lighting-level-light-share.md)
- [History](../../history/night-lighting-level-light-share.md)
- [Room light maps](../../engine/room-light-maps.md), [Light objects and rigs](../../engine/light-objects-and-rigs.md)
- [Walls](walls.md), [Floors](floors.md), [Objects and rigs](objects-and-rigs.md), [Rooms at Night](unlit-rooms.md),
  [Lot light pass](lot-light-pass.md)
- [Light through stairwells](stairwells.md), [Stairs on the directional maps](stairs.md),
  [Light through arches and doorways](arches.md)

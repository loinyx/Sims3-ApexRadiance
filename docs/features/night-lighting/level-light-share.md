# Light between stories

Lamps light every story of a house, not only the story they stand on. An outdoor sconce on the upper wall also lights the
wall below it, with no straight cut at the floor line, and garden lamps keep lighting the upper stories. Indoor lamps
shine through stairwells, atriums and removed floors into the rooms above and below, and the walls of a double-height
room meet at the floor line without a step in the light. Lamps never shine through a solid floor or around the walls of
their own story. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Outdoor and indoor light between stories: released (present since 2.1.0, the first version in this repository). Indoor light through openings across more than one floor, the raised-room wall veto, structure-change refresh and the *Wall seams.csv* recording: in development (PR #2) |
| Default | On (all four switches) |
| Menu | Lighting > Stories (*Seamless walls between floors* and *Every floor in full detail* under Advanced > *Floor detail*) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/level_light_share.cpp`](../../../features/level_light_share.cpp), [`features/lamp_mark_filter.cpp`](../../../features/lamp_mark_filter.cpp), [`features/room_ambient_policy.h`](../../../features/room_ambient_policy.h), driven by [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) |

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

Apex Radiance adds lamps of other stories to the game's own room light lists, with the same weight the game gives them
on their own story, and tests every borrowed lamp against the floors and walls it must pass, with the game's own wall
test. The light solve itself, lamp ranges and attenuation are the game's.

1. **Outdoor sharing.** After the game gathers room 0 of a story (0 to 7), the outdoor lamps of every other story are
   added with the game's gather function.
2. **Refresh cascade.** A change in room 0 of any story refreshes room 0 of all stories, so switching an upper lamp also
   updates the stories below.
3. **Walls of the lamp's story.** A borrowed outdoor lamp is tested against the walls of its own story and of every
   story in between, mirroring the game's per-batch wall culling. A porch lamp under an upper overhang is not affected.
4. **Indoor openings.** An indoor room near a removed floor takes the lamps of rooms on other stories that are near the
   same opening. Every point of the light map is lit by such a lamp only when the real ray from the lamp passes through an
   opening in every floor it crosses, then past the walls of the lamp's room and of the stories in between.
5. **Detail and ambient.** Rooms seen through an opening get the camera story's lighting detail. Rooms joined by removed
   floors into an atrium share one ambient colour and normalisation.
6. **Seamless walls.** Wall samples are lit at the height the wall mesh draws them, and atrium walls are blurred across
   the floor line as one wall.
7. **Updates.** Lamp changes, floor edits, wall or roof changes, lot rebuilds and world loads send exactly the affected
   rooms to gather again, without loops and without re-solving rooms whose lamps did not change.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Outdoor light between floors | `luzExternaEntreAndares` | bool | on | | Shares outdoor lamps between stories (parts 1 to 3). Installs or removes the whole module live; the other three switches need it |
| Indoor light between floors | `luzInternaEntreAndares` | bool | on | | Indoor lamps light other stories through openings (part 4). A change sends every lot's rooms near openings to gather again |
| Advanced > Seamless walls between floors | `paredesSemEmendaEntreAndares` | bool | on | | Wall samples at their drawn height and atrium walls blurred across the floor line (part 5). A change relights every room |
| Advanced > Every floor in full detail | `todosOsAndaresEmDetalhe` | bool | on | | Every room of the played lot gets the top lighting detail on every story, so changing floors keeps the light. More solve work when entering a lot. A change relights every room |

All four are applied live (`LevelLightShare::Install`/`Uninstall`, `SetIndoor`, `SetWallAlign`, `SetAllFloors`) and
reset to on. There is no strength: borrowed lamps keep the game's weight. The module is installed only while Night
Lighting is on. The *Lighting > Stories* card also hosts *Every-Story Ground Light* (`SplitLevelGroundLight`), a separate
patch.

## Compatibility and interactions

- **Walls and floors** ([walls.md](walls.md), [floors.md](floors.md)) read the room maps this module changes; the
  outside wall gain multiplies the result.
- **Objects** ([objects-and-rigs.md](objects-and-rigs.md)): rigs already take outdoor lamps of every story (room id 0
  matches everywhere), so walls and windows now agree on the lamp list. A rig still uses the 3 strongest lamps at the
  object centre without wall shadow; the map sums every lamp per point with wall occlusion. Object rigs only take lamps
  of their own room, so an indoor lamp added to another room's list never reaches that room's objects.
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

## Limitations

- A balcony slab of the upper story does not block outdoor light going down: occluders are per story. The same holds for
  type-11 lights under the Split-Level fix.
- The cross-story wall test runs only on the light tree thread. Points the game solves on another thread get no
  cross-story occlusion (status *on another thread: N*).
- Basements (stories -4 to -1) keep the game's lighting: no sharing, no cascade. Stories above 7 do not exist in the
  tracker.
- Indoor sharing reaches only rooms and lamps within 8 m of an opening, and at most 64 borrowed lamps per room, nearest
  stories first.
- The receiving room's own walls are tested by the game over the whole ray. For a lamp below the room, a wall of the
  room may block the part of the ray that is still under the floor. A lamp above, the common case, is exact.
- The horizontal sample offset of wall texels is left as the game has it, so a very slight step can remain at wall
  corners. Where a real floor strip separates two walls, the step at the floor line stays by design.
- After a load or a lamp switch, rooms are solved one at a time by the game's queue, so a room can show its previous
  light for a moment. The menu notes that changing floors forces the shown rooms to update.
- The game's other basis-map readers (stairs, instanced objects drawn by game shaders) read the directional maps as
  the game builds them, without Apex's per-object cap.
- Steam 1.67.2 only. The optional parts (detail boost, ambient merge, normalisation guard, directional-map guard,
  seamless walls) each install only when their bytes match and otherwise leave the game's behaviour.

## Technical reference

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
   story, `IndoorBoundaryPass` finds the ray's crossing of that floor (the lowest floor height first, then the height of
   the tile it lands on). The quadrant there must be an opening over an indoor room of the story below, else the lamp
   gives nothing. Points within 2 cm of the floor plane are tested at their own place.
2. The crossings must be strictly ordered along the ray. Wall segments are built from the actual crossings, not from
   nominal story planes: ghost wall rows and split-level tile heights can place a sample on the lamp's side of a boundary
   despite its room's story number. The segment of the story holding the ray's endpoint is kept when it is not the
   receiving story.
3. When the game tests 2D walls in this batch (`flags[0]`), each segment is tested with `FUN_0069fc40` against the room
   of that story found at the segment's midpoint (the lamp's own room for the first segment), from the segment's start
   to its end, with `+0x639` swapped as in part 3. Transmissions multiply (clamped 0..1). The receiving room's walls are
   tested by the game.
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
  signatures, both resolving to `0x006C7BA0`.
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
  lot due after 120 ms for a pure on/off change or 700 ms for a move or room change (`LampRefreshDelay`), then
  `LampMarkFilter::OnPresent` runs `LevelLightShare::RelightLot` (every room of the lot, stories -4..7, room 0 too) and the
  rig refresh, at most once per 2 s per lot (`kLotGap`). Nothing happens while the night level is between 0.02 and 0.98
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
- Samples are recorded only while armed (Developer > Lighting *Record story light samples for the diagnostics*, or the
  first F8 of a session). The game's wall test is wrapped (`GameWallTest`, CALL at `0x0069FE93`) only in developer
  mode, to record its result.

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

The point solve runs for every texel of a room light map. The hooks read the thread id from the TEB and do nothing for
rooms without borrowed lamps. Diagnostic sample recording exists only in developer mode and runs only while armed. Every
floor in full detail adds solve work once when entering a lot. In-game cost has not been measured (see
[validation](../../validation/night-lighting-level-light-share.md)).

## Rejected approaches

- Sharing lamps without a wall test: lower side walls near corners became brighter than the upper ones. Details in
  [history](../../history/night-lighting-level-light-share.md).
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

## See also

- [Validation](../../validation/night-lighting-level-light-share.md)
- [History](../../history/night-lighting-level-light-share.md)
- [Room light maps](../../engine/room-light-maps.md), [Light objects and rigs](../../engine/light-objects-and-rigs.md)
- [Walls](walls.md), [Floors](floors.md), [Objects and rigs](objects-and-rigs.md), [Rooms at Night](unlit-rooms.md),
  [Lot light pass](lot-light-pass.md)

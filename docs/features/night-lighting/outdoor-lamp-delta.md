# Quick outdoor lamp edits

Moving, recolouring, dimming or switching a garden lamp changes its light on the outside walls, decks and lot grass of
every floor within a few frames, instead of after every floor's outside has been solved again with all of its lamps. The
light is the same as after a full solve. This part runs only with Lamp lighting: Instant (GPU), the default, which
takes its base from the parts kept here ([Real-time outdoor lamps](rt-outdoor-lamps.md)).

## Status

| | |
|---|---|
| Availability | Released in 2.13.0, as part of Performance > Lamp lighting > Instant (GPU) Released in 2.18.0: a refused delta keeps the story's record as the GPU base. |
| Default | On (Lamp lighting: Instant (GPU)); with Classic every room 0 is solved in full |
| Menu | No switch of its own (System > Performance > Lamp lighting); its check: Developer > Lighting > Compare lighting paths > Check quick lamp edits |
| Configuration | `lampLightingMode`, `outdoorLampDeltaCheck` under `[patches.NightTerrainRelight]` in `ApexRadiance.toml` |
| Source | [`features/outdoor_delta.cpp`](../../../features/outdoor_delta.cpp), [`features/outdoor_delta_model.h`](../../../features/outdoor_delta_model.h), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (`ReplayBatch`, the solve hooks) |

## The problem

The outside of each lot story is its room 0, lit on the CPU texel by texel with every light of its list
([engine/room-light-maps.md](../../engine/room-light-maps.md)). An outdoor lamp is in the list of room 0 of every story
([level-light-share.md](level-light-share.md)), so one lamp edit sends every story's room 0 to be solved again with all of
its lights, story by story.

## How Apex Radiance solves it

A room 0 texel is the plain sum of what each light of the list added at it, packed as `min(sum, 1)` with its hue kept past 1
([level-light-share](level-light-share.md#outdoor-texels-keep-their-hue)). When room 0 of the
active lot is solved, Apex keeps each light's part of every texel, read from the solve's running sum after the game and
Apex tested that light. A lamp edit then brings every story's room 0 up to date in the frame of the edit:

1. **Kept parts.** During a full solve of room 0, each batch sample's sum and each light's part of it (and, on the floors
   of upper stories, its part of the floor mask's two copies) are kept with the sample, its batch and its texel. A texel is
   read back from its map after the game packed it; class-2 wall blocks are kept as they were before the wall blur.
2. **The edit.** A lamp of room 0 that moved or changed a value in place reaches the game's lamp entry update. When every
   story's room 0 of the lot is solved, shown at the class it was solved at and has its parts kept, the mark is dropped.
3. **Each changed light alone.** The game's batch solve runs again over the kept batches within the light's reach, with
   the room's light list holding only that light for the call (`LevelLightShare::ReplayBatch`).
4. **The texels.** Every texel it reached takes `sum - old part + new part` and is packed as the solve packs it; a
   touched class-2 wall block is blurred again from its texels before the blur.
5. **The maps.** Only a story whose texels change is written, through the room's own lock step and the finalize's own
   steps. With Real-time outdoor lamps covering the lot, a change of only GPU-drawn lamps writes nothing.
6. **The rooms that take them.** Room 0's yards and the roofed rooms that saw a door of a changed light through their
   openings (of the story and of the story above) are sent once after the delta (`SendRoom0DependentsTail`), the roofed
   rooms at a lamp edit's priority: the delta marks no room, so the game's walk does not send them.

Anything else is solved in full, and its parts are kept again: a first solve, another detail class, changed walls,
floors or openings, a lighting option changed, more than eight lights changed, a quick pass, a solve off the light tree
thread.

The room 0s of the lots next to the active one that Real-time outdoor lamps relights with the active lot's posts
(`SetNeighbourLots`, at most four) keep their texels and sums only, every light of the list kept with no part
(`Record::baseOnly`): their sum is the GPU's base. No delta takes them (an edit there is a full solve), and the memory
budget drops them first; they make room only for one another.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Lamp lighting > Lamp updates: Instant (GPU) | `lampLightingMode` | string | `"instant"` | `"classic"`, `"instant"` | Runs this part with Real-time outdoor lamps, only where they can run (an unsupported device with Instant chosen works fully as Classic); Classic leaves every room 0 to its full solve |
| Check quick lamp edits | `outdoorLampDeltaCheck` | bool | off | | Developer only, shown in Instant. A room brought up to date is solved in full again 1.5 s after its last delta; the log gives the largest texel difference between the two |

## Limitations

- Runs only with Lamp lighting > Instant (GPU), and only on the active lot (the lots next to it keep their base only).
- The first edit after a room 0 was solved without its parts kept (a load, another class, an option change) is a full
  solve; the edits after it are quick.
- A texel can differ from a full solve by one 8-bit level (rounding of the kept parts).
- The kept parts take memory: up to 96 MB for every room together and 160 MB for the active lot's room 0s (pinned by
  Real-time outdoor lamps), kept above 2 GB of the address space when possible; the oldest rooms of other lots are dropped
  first. In a pinned room the parts of the lamps the GPU draws are kept as one sum per texel (`CollapseDrawn`: the
  GPU reads only the base, the texel's sum less them; exact), and a pinned room never drops another pinned room: past the
  ceiling the new solve is simply not kept (`not kept` log line with what fills it) and Real-time outdoor lamps stops
  sending that story to its solve, so the stories do not evict one another in a loop. A delta that needs a summed lamp's own parts (a drawn and
  a baked light changed together, or the room no longer covered) solves in full. `Memory budget tight` is logged once each
  time the kept parts pass 85% of the ceiling, split into samples, texels, baked lights' parts, drawn parts and sums;
  Developer > Tester > Frame time shows *Instant (GPU) kept data* (MB kept / ceiling, rooms).
- While a lamp is edited, a delta with more than 200 batches to solve again is refused: the room takes its full solve.
  After such a refusal, or one for more than eight lights changed, the room's record stays as Real-time outdoor lamps'
  base only, marked stale, so its story keeps the GPU's lamps until that solve ends: no delta, direct lot update or
  self-check uses it, and the solve's own parts replace it, or it is erased when they cannot be kept. A change of the
  walls, floors or openings, or of the lighting options, erases it at once.
- (2.15.3) With *Indoor light adds to the ground* active, a delta whose changed light is a roofed room's lamp that story 0's
  room 0 took through an opening is refused: the ground texels' alpha holds that lamp's light too
  ([level-light-share](level-light-share.md#indoor-light-on-the-ground)).

## Technical reference

| Part | Where |
|---|---|
| Kept parts | `SolvePointBatch` (`OutdoorDelta::Sample`, `Tap`, `Mask`, `SampleEnd`), `LightEvalHook` (`OnEval`), the texel read back at the next sample, before the wall blur (`BlurWalls` -> `NoteWallBlur`) and before the finalize (`FinalizeHook` -> `BeforeFinalize`) |
| A light alone | `LevelLightShare::ReplayBatch`: `FUN_006a31d0` on the kept samples (the batch vector `0x01158AC8` and room+0xC8 / +0xCC swapped for the call) |
| An edit taken | `LevelLightShare::HoldLampMark` -> `OutdoorDelta::TakeEdit`; at the end of the room update `RoomUpdateHook` -> `AfterRoomUpdate`; the dependents `SendRoom0DependentsTail` |
| Maps written outside a solve | `[0x011D0508]` = 0, the lock step `0x0069FA40`, then `0x006A8CA0` / `0x0069EFC0`, `0x0069E950(1)`, `0x006A08E0` |
| Delta in the lock step | `LockStepHook` -> `OutdoorDelta::AtLockStep`; returning 0 at `0x006A3D2B` sends the solve to its finalize |
| Validity | the room and class, the lighting options (`LevelLightShare::PhysicsKey`), the lot's geometry (`LotGeometryKey`), the threshold and wall mode, the maps' textures and sizes |
| Log | `[OutdoorDelta]` lines per delta, per full solve and per edit; a solve whose parts are not kept names the key that changed during it (walls, floors or openings; the light rules, `PhysicsKey`; the lighting options) |
| Offline check | `tools/outdoor_delta_test/run.ps1` (deltas against full solves of a model) |

## See also

- [Real-time outdoor lamps](rt-outdoor-lamps.md), [Level light share](level-light-share.md)
- [engine/room-light-maps.md](../../engine/room-light-maps.md)

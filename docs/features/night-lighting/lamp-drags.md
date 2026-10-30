# Lamp drags

While a lamp, or the object holding one, is dragged in Build or Buy mode, Apex Radiance keeps the room solves a drag
causes to the lamp's own rooms and spreads the rest over the frames after the lamp stops.

## Status

| | |
|---|---|
| Availability | Released in 2.13.0. Fast own-room relight, let go, the own room at once, the furniture with its room: In development (branch `fix/2.13.1-indoor-speed`) Released in 2.18.0: a lamp drawn on the GPU while dragged takes no local relight. |
| Default | On; each measure has a switch on Developer > [Tester](../dev-tools/tester.md) |
| Source (2.13.1) | Also [`features/atrium_hold.cpp`](../../../features/atrium_hold.cpp), [`features/level_light_share.cpp`](../../../features/level_light_share.cpp) (`OwnRoomsPending`) |
| Menu | None (developer switches only) |
| Source | [`features/drag_lite.h`](../../../features/drag_lite.h), [`features/drag_lite.cpp`](../../../features/drag_lite.cpp), [`features/room_light_queue.cpp`](../../../features/room_light_queue.cpp), [`features/lot_lighting_motion.cpp`](../../../features/lot_lighting_motion.cpp), [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) |

## The problem

Without these measures, a drag diagnosis (27.9 s, 1,071 frames) showed 26.0 ms frames against 8.46 ms before the drag: 1,173 room
solves over 13 rooms, 2,315 fan-out sends, a 15 ms solve budget in every frame and a 215 ms frame after each drag (the
game's terrain countdown, armed again by every step). The rooms lit through openings and from other stories were sent
again at every step of the lamp.

## How it works

The drag is seen from the lamp's own change stream: position steps of a lamp entry (`LampMarkFilter`), lamp marks
(`LevelLightShare::NoteLampEditing`), lamp moves seen by the lamp list, and a type-11 light removed and added again on the
same lot within 0.5 m (`LotLightBridge::TrackLotLampEdits`). A step within 750 ms is a drag (`DragLite::Engaged`);
`LevelLightShare::LampDragging` counts every step.

| Measure | What it does |
|---|---|
| Deferred extra sends | Apex's extra room sends (the fan-out to rooms behind openings and on other stories, room 0's dependents, the lifted-lamp retries, the openings' door checks, the audits) wait and are sent once each when the lamp is still, with no urgency. Sends that stop a solve (a removed lamp) and the game's own list rebuilds are never held |
| Re-pick throttle | The room priority call gives priority 0 to a room that takes the lamp's light from elsewhere (urgency tier 1 or 2) until the lamp is still, to a room whose solve began less than 400 ms ago, and to any room within 120 ms of the last solve start. The call goes through Apex also while Faster room lighting is off (`RoomLightQueue::EnsureDragThrottle`) |
| Spread cap | After the stop, while the edit's rooms still wait (at most 15 s), the frame's room solves stay under 3.5 ms: Faster room lighting's drain takes what it needs up to the cap, the lot being played the rest, the other lots 0.25 ms; no edit boost |
| Terrain countdown | At night, with the local terrain path available, the countdown a drag arms (cells+0x38) is put back to idle and the moved lamps are relit locally, but only while the local relight has taken a lamp change within the last 3 s (`kDragIdleProof`; a change drawn on the GPU while dragged counts); otherwise the game rebuilds the terrain as it would without Apex |
| Slower refresh | Lamp list re-reads every 250 ms (33 ms otherwise) and local terrain relights every 250 ms (100 ms) while dragged; the read and relight after the last step are the exact ones. A lamp whose ground light is drawn on the GPU while dragged takes no local relight during the drag ([terrain relight](terrain-relight.md#dragged-lamps-on-the-gpu)), and casts no wall shadow on the ground while it moves; its shadows come back with the shade built after the drag ([lot light pass](lot-light-pass.md#lamp-shade-on-lot-ground-groundshadow)) |

The log line `[DragLite] Lamp drag ended` gives the solves during the drag, the re-picks held, the sends left out and
sent, and the average and worst frame of the drag.

### Fast own-room relight

Tester switch *Fast own-room relight after edits* (`fast_own_relight`, on). Added in 2.13.1: in 2.13.0 the spread cap
also held the lamp's own rooms, so a drag's stop took 9 s over 810 frames (5.0 ms of solves a frame) to settle, and with
Lamp lighting Instant an indoor lamp, which the game still solves, took the outdoor drag rules (2,519 re-picks held in one
drag).

- **Own rooms first, after any edit.** While a room the edit marked with urgency tier 0 (the lamp's own room and the rooms
  of its story that hold it, `LevelLightShare::OwnRoomsPending`) still waits for its solve, there is no spread cap: the
  lot pass and Faster room lighting's drain keep their normal edit budgets (25 ms and 12-16 ms with the camera still);
  other lots keep 0.25 ms, as under the cap.
  Only the rooms around the lamp (the deferred sends, other stories, the rooms lit through openings) are solved under
  the 3.5 ms cap once the own rooms are done.
- **A value is not a drag.** A colour, intensity or switch change no longer starts the Classic hold; only moves do.
- **Instant, indoor lamps.** A dragged lamp the GPU does not draw (an indoor lamp, or an outdoor lamp the Instant path
  does not cover) follows the Classic rules: no solve during the drag, its own rooms first at the release, and *show all
  floors together*. The lamps the GPU draws keep the measures above.
- **All floors together.** Once the own rooms are solved, the hold waits at most 300 ms more for the others, then shows
  what is ready; the rest change as each is solved.
- Log, one line per edit with own rooms: `[DragLite] Lamp edit relight: the lamp's own rooms (N) solved after T ms, the
  rooms around them settled U ms after the edit; F frames, C under the spread cap`. If an own room is still not
  solved 3 s after the edit, one more line gives the drag hold and spread cap state (log only).

### The lamp's room together with its furniture, at the release

Added in 2.13.1 after a recording of an indoor lamp dragged on story 2 (Lamp lighting Instant, so the Classic rules):
the lamp's room showed its new light 0.69 to 1.05 s after the drag's first step, while the furniture had followed the lamp
from +0.00 s. Of the 0.8 s between the last step and the new light, the room's solve took 31 ms (S to E); the rest was the
400 ms still time, 200 ms more of `LampDragging`, and the 300 ms grace of *show all floors together*. The quick pass was
never used: it is off while *Lamp edits all at once* is on, and drags and value edits never take it. With the solve
itself at 31 ms (two frames) it is not needed, and it would add a change of its own (class 0 against class 2).

- **Let go** (Tester switch *Relight when the lamp is let go*, `drag_let_go`, on): the primary mouse button, polled once
  a frame while the game's window is in front, going up after a move made while it was down ends the drag at once
  (`DragLite::LetGo`): `ClassicHolding`, `Engaged` and `LevelLightShare::LampDragging` end in that frame, so the own rooms,
  the left-out sends and the local terrain relight (`EditReady`, 50 ms later) start then. Steps within 300 ms after it
  (`kPlaceGraceMs`: the placement, the lamp list's last read) do not start a new drag; a later step does. A lamp moved with
  the button up (picked up by a click, rotated with the keys) keeps the 400 ms still time.
- **The own room at once** (Tester switch *Show the lamp's room as soon as it is solved*, `own_room_at_once`, on): the
  hold shows the own rooms in the frame after their solve ends (the 300 ms grace is 0; still at least 150 ms after the
  latest edit). Any hold that ends (not at the cap) keeps the maps of a room whose solve has not ended since its mark until
  it has (at most 8 s; "trailing"), and keeps the maps that room's solve locks later the same way: a solve thrown away by
  an invalidate (the game unlocks its maps half written) or one begun before the mark is never shown.
- **The furniture with its room** (Tester switch *Furniture changes with its room*, `furniture_with_room`, on): every lamp
  edit, not only a switch, holds the furniture's rig lights until its room's maps show the edit
  (`AtriumHold::RoomShowsOldLight`). Before, the snapshot of the lights had to be older than the latest switch, so a move
  or a colour never found one ("held for a lamp switch 0" in the recording). The snapshot is now from a frame before the
  edit began; a part whose room is marked later and that already showed new lights is left as it is (holding it would turn
  it back).
- Logs: the Classic drag line ends with "ended when let go, N ms after its last step" (or "by the still time"); the
  `[AtriumHold]` line adds "shown N ms after the lamp was let go", the map writes kept back and the maps still trailing;
  `[LotLightBridge] Lamp edit furniture: N object parts kept their lights ... shown with it T ms after the edit (C light
  changes of theirs not shown; L parts ... left as they were)`.

Expected: the own room and its furniture change together 60 to 150 ms after the button is let go (one frame to see it,
the solve's two or three frames, at least 150 ms after the latest step). The rooms around the lamp (other stories, room 0,
through openings) still follow under the spread cap, each keeping its light until its own solve ends.

### Classic: relight only after release

Tester switch *Classic: relight only after release* (`classic_after_release`, on), Lamp lighting Classic (or
Instant on a device that cannot run it); Instant keeps the measures above unchanged. While a lamp is dragged no room
light work happens at all, as in the unmodified game, and every lamp-light update is done once after the release:

- Every room the drag touches (any urgency tier: the lamp's own room, its story's room 0, the neighbours, the rooms lit
  through openings, the safety net's) and every room of the lot being played gets priority 0 in the priority call
  (`DragLite::HoldUntilRelease`): none is picked, so none is solved. Its state, invalidations and gathers stay.
- The game's light-list rebuilds and invalidations are never held or replayed (holding them broke lamps before): only the
  solves wait, and the lists stay correct.
- The drag ends when the lamp has been still for 400 ms (`kReleaseMs`) or is let go. A run of steps with no such pause
  that lasts more than 20 s (`kMaxHoldMs`) no longer holds: the rooms are then solved as the lamp moves, so a lamp that
  keeps moving never leaves the lot's rooms unsolved. After the release the rooms are solved once:
  the lamp's own story first with Faster room lighting's edit budget (12-16 ms a frame, no spread cap while one of its
  rooms still waits), then the rest under the 3.5 ms spread cap. Chosen so the lamp's own light appears within a few
  frames of the release while the other stories do not spike the frame.
- The local terrain relight (`EditReady`) and the per-pixel lamp list (roofs, water, outdoor objects, trees) wait for
  the release too; the lamp list is still read every 250 ms for the drag's own detection (a type-11 light registered
  again), without rebuilding the per-pixel list.
- The sends left out go at the release; with *Classic: show all floors together* they are tagged urgent (tier 2) so they
  change in the same frame as the rest.
- One log line per drag: `[DragLite] Classic drag: N rooms deferred during the drag, M solves at the stop, settled T ms
  after the release` (or "rooms of the edit still waiting (timeout)" after 15 s).

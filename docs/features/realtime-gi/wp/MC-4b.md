# MC-4b: MC-4's two shortcuts removed

Status: **done offline**; the in-game smoke is open. Branch `gi/mc-4b` (off `feature/realtime-gi` 92d21db, which holds MC-4).
Follows [MC-4.md](MC-4.md). No bridge patch changes, no new game address.

Marks: [V] verified (offline test, build or disassembly named), [I] inferred, [O] open.

## 1. Kept rooms marked again without their solve

MC-4 left this open: a kept room's blocker table took the tiles' set as of its solve, so an object whose mesh reached the
tiles later was darkened twice (its mesh and its game blockers) until the room's next solve; an object leaving the tiles
lost its shadow the same way.

| Part | Where | What |
|---|---|---|
| Mark generation | `features/mesh_caster_set.h` (`Set::MarkGen`) | Moves when `InTiles` or `ServerCasts` may answer differently: an object added, its LITE changed or removed; a LITE resolved for the first time; an upload (`AppendTo`) whose tiles' object set differs from the last one. A move inside the tiles does not move it |
| `ObjectLegs::MeshGen` | `features/object_legs.cpp` | The set's mark generation with the tiles and per-pixel switches folded in (turning either on or off marks again too) |
| `ObjectLegs::MeshedKey` | `features/object_legs.h` | The marked records' indices, hashed: a table differs from its solve's when this does |
| Kept rooms | `features/outdoor_delta.cpp` (`KeptRooms`, `LiveTable`) | Every physical kept room is due when `MeshGen` moved, also with `liveObjectTables` off (developer default). With no object moved, its solve's records are marked again (`ObjectLegs::Fill`); a different mark set publishes a live table (`KeptRef::live`), the same set draws the solve's table again. With moved objects (live tables on) the moved records are marked as before, and the live key holds both the moves and the marks |

- [V, code] The live key the GPU side compares (`gpu_room_lamps.cpp`: `r.live != e.liveKey`) changes whenever the marks
  change, so the new table is taken the next frame; a mark change alone does not hold the solve (`NoteLiveMove` only on
  object moves).
- [V, code] Without mesh casters `MeshGen` stays put, so a room is read once per kept version (one `Fill` of its records)
  and then only when its own moves change, as before.
- [I] Cost: a mark generation change refills every physical kept room once (records only, no game reads beyond `Fill`'s
  maps). A lot load moves it often; at most one refill per room per frame.
- [O] Room 0's table at its solve start (`RecordRoom`'s `r.blk`) still waits for the solve, as the live tables do (the
  existing shortcut there).

## 2. The feature turned on inside a world

MC-4 left this open: objects attached before the sink was on stayed unknown until they attached again.

Hook check [V, disassembly of `TS3W.exe` 1.67, capstone]:
- `LightObjectAttach` (0x006B1430) inserts the object id into the light manager's hash map at `[this+4]` (0x006B0E10) only
  when the LITE resource resolves, and releases the resource at the end: the node keeps the lights built from it, not the
  LITE key [I: not written in 0x006B1430 itself; the builders 0x006B0EF0 / 0x006B1190 not read]. A walk of that map at
  enable time would give object ids without their LITE, so it cannot replace the hook's record.
- `LightObjectMove` (0x006B09C0) looks the id up in the same map (0x00A9E1D0) and returns 0 when it is not there: a move
  the hook records is always of an attached object, so the learnt objects are at most the manager's.

Fix: `ObjectLegs::Feed` learns every object while the hooks are in and developer mode is on (meshCasters is a developer
switch), also with the sink closed (`Feed(false)` / `SetOn`: nothing goes out then). Opening the sink
(`ObjectLegs::SetSink(true)`, RealtimeGI's first frame with meshCasters on) sends every learnt object once and logs
`[GiMesh] N objects known as the sink opened, M sent`. Closing it (RealtimeGI off) drops the pending output and keeps the
objects known. Shapes (`NoteShape`) and removals are learnt the same way.

- [O] Developer mode itself turned on inside a world still misses the objects attached before it (marked as a shortcut in
  `object_legs.cpp`): Public builds learn nothing until the sink opens, so they pay no memory for a switch they do not have.
- [I] Memory: one map entry (about 100 B) per attached LITE object in developer mode, at most 65536 (`Feed::kMax`).

## Counters

- `[GiMesh] N objects known as the sink opened, M sent`: the enable-time seed.
- `[LiveTable] Story S room R: its records left to the mesh casters changed ahead of the solve`: the first re-mark of a room.

## Tests

| Test | Result |
|---|---|
| `tools/rt_lamps_test` `MeshMarkChecks`: objects learnt with the output off, nothing sent, the opening sends the complete ones once (not a removed one, not one without a matrix), shapes learnt; the mark generation moves on add, resolve, into/out of the tiles, removal, and not on a move inside the tiles; a record re-marked after its solve gives another `MeshedKey`, back out gives the solve's key | [V] 680,496 checks, 0 failures |
| Developer and Public builds (msbuild `/m:2`) | [V] pass, no compiler warnings |

## What the in-game smoke must check

- `mesh-casters-smoke` (both variants) as for MC-4, plus: turn RealtimeGI meshCasters on inside a loaded lot with
  developer mode on: the `[GiMesh] ... known as the sink opened` line shows N > 0 and M > 0, and still objects cast.
- With meshTiles on: move a lamp-lit object into the lot (or buy one) without a room re-solve; its record should be re-marked
  (`[LiveTable] ... left to the mesh casters changed`) and the object darkened once (no doubled shadow, no lost shadow).

## Unblocks

The in-game smoke of MC-2/MC-3/MC-4/MC-4b together.

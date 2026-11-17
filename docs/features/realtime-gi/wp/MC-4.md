# MC-4: each object darkened once, still objects seeded

Status: **done offline**; the in-game smoke (`mesh-casters-smoke`, both variants) is open. Branch `gi/mc-4` (off `gi/mc-3`).
Follows [MC-3.md](MC-3.md) and [MC-2.md](MC-2.md). No bridge patch changes.

Marks: [V] verified (offline test, build or disassembly named), [I] inferred, [O] open.

## 1. Double darkening

With `meshTiles` on, an object whose own mesh is in the physical indoor lamps' shadow tiles was also tested by its 3D
record (the game's blocker rectangles and discs, copied into the GPU's blocker table). Now:

| Part | Where | What |
|---|---|---|
| Tiles' set | `features/mesh_caster_set.h` | `AppendTo` notes the objects whose triangles it put in the tiles; `InTiles(id)` |
| Record link | `features/object_legs.h` (`Feed::Shape`, `MarkMeshed`) | LampMarkFilter's occluder mark (object id, first shape) links every LITE object's first shape, not only the legged tables'; a 3D record names its object by that shape (`Record::shape`) |
| Mark | `ObjectLegs::Fill` (all three call sites in `outdoor_delta.cpp`) | A record whose object is in the tiles gets `Record::meshed` and no legs; unresolved objects (missing, over 2,000 triangles, over budget, holding a lamp, past the triangle cap) keep the game's blockers |
| Table | `features/rt_lamp_blockers.h` (`BlockerTable::Append`) | A meshed record keeps its rows (record order and shape list unchanged) with zero blockers |

- [V] The records as read are unchanged: only the drawn table leaves out a meshed record's blockers, so the room keys,
  live moves and the CPU replays compare what they did before.
- [I] The meshCasters-only server path (no `meshTiles`) has no blocker double: the server's rays are a separate term and
  the client does not know which LITEs the server resolved. Not gated.
- [O] A kept room's table takes the tiles' set as of its solve (or its last live move): an object whose mesh reaches the
  tiles later is darkened twice until that room's next solve. Marked as a shortcut in `ObjectLegs::Fill`.

## 2. Still objects

Where the hooks come from [V, disassembly of `TS3W.exe` 1.67, capstone]: `LightObjectAttach` (0x006B1430) and
`LightObjectMove` (0x006B09C0) each have one direct caller, the same message handler 0x006B1850 (vtable slot at
0x00FF3C7C): message 0x1361A133 calls the attach with the message's object id and LITE key, message 0x3361F6C9 the move
with the matrix built from the message. So every object's attach and placement are messages; their order on load is
not fixed by this code [I].

- Before: an object went out only on a move after its attach (a matrix before the LITE was dropped) [V, code].
- Now `ObjectLegs::Feed` keeps each LITE object's LITE and latest matrix (at most 65536, objects without a LITE dropped
  first); an object goes out once both are known, in either order.
- Seeds: every known object goes out again when the bridge's GI interface opens, at world entry (first world frame) and
  at each lot entered (the active lot changes). The server and the tiles' set keep the latest record of each object, so a
  seed is idempotent.
- [O] An object attached before the sink is on (the feature turned on inside a world) stays unknown until it attaches
  again: no object-list walk was needed for the normal start (the feature starts at the first frame, before a world
  loads), so no new game address.

## Counters

- `[GiMesh] ... dropped= seeded=`: objects sent by seeds, summed.
- `[GiMeshTiles] ... tileTris= meshedRecords=`: 3D records left to the tiles, summed over fills.

## Tests

| Test | Result |
|---|---|
| `tools/rt_lamps_test` `MeshFeedChecks`: matrix before LITE, LITE before matrix, no LITE; still objects send nothing more; a synthetic load (empty set, no move) filled by the seed (seeded 2, set asks for the model); only the tiled object's record leaves the 3D test; the drawn table gives it 0 blockers and the missing and unknown objects' records 1 each; a removed object's record unmarked once the tiles drop it | [V] 680,478 checks, 0 failures |
| `tools/ingame_runner/jobs/mesh-casters-smoke.json` | Both variants expect `seeded=` over 0 on the `[GiMesh]` line; the patterns were checked on sample lines (seeded=40 matches, seeded=0 does not) |
| Developer and Public builds; runner selftest (561 checks, 0 failures); i18n check (OK); menu audit; public text check | [V] pass (three older comments of `room_light_tests.h` lost their recording dates for it) |

## Unblocks

The in-game smoke of MC-2/MC-3/MC-4 together (bridge pair with 0020 and 0022, physical indoor lamps); S5's exact
furniture on the Direct3D 9 tier without a double shadow.

# World lamp response

Street lamps that belong to the world rather than to a lot (the posts along roads) update the ground and nearby objects
when they are recoloured, dimmed or switched, in the same way lot lamps do. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 2.5.6. Rig request on early terrain completion and world-change clearing of the direct lamp pool: in development (PR #2) |
| Default | On (part of *Lot lamps light the street* tracking; no own setting) |
| Menu | None |
| Configuration | None |
| Source | [`features/world_lamp_policy.h`](../../../features/world_lamp_policy.h), lamp tracking in [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp), edit handling in [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) |

## The problem

F8 captures identify the affected posts as type-11 lights with lot id 0. The lamp reader rejected every lot id 0, so
neither the change tracking nor the bake snapshot of [terrain relight](terrain-relight.md) saw their edits: the ground
and the native object rigs kept the old colour until some unrelated rebuild.

## How Apex Radiance solves it

1. **Admit world lamps.** `ReadLotLamp` admits live world-owned type-11 lamps in the existing enumeration; other world
   light classes stay excluded and lot-owned rules are unchanged.
2. **Accept only edits.** World lamps share snapshot group 0; observed value edits of known lamps are priority edits,
   including many lamps recoloured at once. Streaming additions and removals are not.
3. **Reconcile terrain and rigs.** The edit goes through the terrain relight's priority route (local relight of the old
   and new footprints, or the full fallback), and, once per coalesced edit, requests a refresh of every native object
   rig.

## Settings

None. World lamps follow the terrain relight settings.

## Compatibility and interactions

- [Terrain relight](terrain-relight.md): debounce (80 ms quiet or 500 ms from the first edit), local queue, limits and
  full-rebuild fallback. `DeferDayEdit` never defers a world-lamp edit by day: the night level does not establish that
  an observed world lamp is off.
- [Objects and rigs](objects-and-rigs.md): `ObjectLightBridge::RequestRigRefresh` invalidates all native rigs on the render
  thread, not only those near the edited lamp.
- [World atlas](world-atlas-and-smoothed-maps.md): a local chunk finished through the native LOD path still reports
  `NoteChunkRendered`, so smoothing does not wait for hash discovery.
- The direct roof, water and object lamp pool reads the same enumeration; a changed list invalidates the exact-position
  selection memo.

## Limitations

- No atomic same-frame update of every surface: terrain chunks follow the local queue pacing, rigs the next refresh.
- A native lit-bit-only dusk/dawn transition is not a priority world edit; it is left to the endpoint rebuild.
- The rig refresh is global; its cost has not been measured.
- Indoor lights are not part of this path.

## Technical reference

`WorldLampPolicy` (`world_lamp_policy.h`):

| Function | Rule |
|---|---|
| `Track(lot, type)` | `lot != 0 || type == 11` |
| `Eligible(lot, type, flags, room)` | Alive (flags & 1), tracked, type 11 or 3..6; world-owned (lot 0): type 11 only, no room-known bit 0x04 or room check (captured world flags 0x73 / 0xF3); lot-owned: 0x04 room known and room 0 |
| `AcceptEdit(lot, added, removed, edited, observed)` | Lot lamps: always; lot 0: no additions or removals, at least one edit, observed |

In `TrackLotLampEdits`, a world type-11 lamp counts as a priority edit when both readings are type 11 on lot 0 and it is
in the bake both times with a changed light, or its enabled bit 0x40 changed, or its intensity crossed zero.

`NoteEdit` records lot 0 among the user-driven lots and sets `g_worldRigRefreshPending`. `RefreshWorldRigs` calls
`ObjectLightBridge::RequestRigRefresh` once when the edit is ready (`EditReady`), independently of terrain completion;
`FinishEdit` consumes the request first, so a terrain rebuild that completes before the debounce does not drop it. A
world change or a new edit batch resets the request.

On world change (`LotLightBridge::OnWorldChanged`) the direct lamp pool, selection memo generation, GPU lamp rows,
enumeration scratch, tracked lamps, lot state, animated count, snapshot, drawn-lot and arrival sets are cleared, and the
next lamp read is requested at once, so new geometry never receives lamp rows from the previous world.

| File | Responsibility |
|---|---|
| `features/world_lamp_policy.h` | World eligibility and priority edit guard |
| `features/lot_light_bridge.cpp` | Enumeration, snapshots, diffs and direct lamp memo; world-change clearing |
| `patches/night_terrain_relight_patch.cpp` | Debounce, terrain decisions, coalesced rig request (`RefreshWorldRigs`, `FinishEdit`) |
| `features/terrain_chunk_relight.cpp` | Local queue completion, including the alternate LOD notification (`FinishFlight`) |
| `features/object_light_bridge.cpp` | Native rig refresh on the render thread |

## Rejected approaches

- Removing only the lot-id-zero exclusion: the room-known gate still rejected the captured world flags.
- A six-second lamp-driven window retry: did not resolve the post delay and was removed.

Details in [history](../../history/night-lighting-world-lamp-response.md).

## See also

- [Validation](../../validation/night-lighting-world-lamp-response.md)
- [History](../../history/night-lighting-world-lamp-response.md)
- [Terrain relight](terrain-relight.md)

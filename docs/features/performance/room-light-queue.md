# Faster Room Lighting

Rooms light up much sooner when you enter a lot, change floors or switch lamps. The lot you are on and the floor you look
at go first, rooms reach their final look in fewer steps, and several small rooms are lit per frame. The final lighting
is the game's own.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (on by default since its introduction) |
| Default | On |
| Menu | System > Performance > Camera and lighting > *Faster room lighting* |
| Configuration | `[patches.RoomLightQueue]` in `ApexRadiance.toml` |
| Source | [`features/room_light_queue.{h,cpp}`](../../../features/room_light_queue.cpp), [`features/room_ambient_policy.h`](../../../features/room_ambient_policy.h), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game solves room lighting one room at a time for the whole world, and picks the next room only on the next frame.
Its priority rule puts every pending first solve of every loaded lot ahead of any upgrade of the story being viewed, and
each room climbs a three-step detail ladder with one full solve per step. From entering a lot to the last room solve can
take many seconds, of which only a small part is solve work: the cause is the queue, not the solve itself.

## How Apex Radiance solves it

Five changes, each checked against the Steam bytes and left off when they differ:

1. **Viewed lot first.** Rooms of the priority lot on the camera's story get the game's priority x4000, rooms below it
   x2000. While Night Lighting's full-detail-all-floors policy is on, every floor of the priority lot gets x4000.
2. **No middle step.** A finished class-0 solve steps straight to the room's target detail class.
3. **Requeues keep the class.** A room solved before goes straight to its target class when it is invalidated.
4. **Several rooms per frame.** After the game's scheduler makes a room of the priority lot current, Apex solves it at
   once and picks the next, until 4 ms are used (1 ms while the camera moves) or a room does not finish.
5. **Stranded rooms.** A room waiting at a class above its current target (its story left the camera's view) gets the
   lowest priority instead of zero, so it is solved last rather than never.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster room lighting | `[patches.RoomLightQueue] enabled` | bool | on | | Turns all five changes on. A missing key reads as on |

## Compatibility and interactions

- **Night Lighting:** its requeues (`QueueRoom`) and the full-detail-all-floors policy of level light share
  ([level-light-share.md](../night-lighting/level-light-share.md)) feed this scheduler. See *Apex's own requeues* below.
- **Spread lot lighting while moving:** independent; both use the shared camera motion signal.
- **Official Sims3SettingsSetter:** its lighting quality patch hooks 0x006A0E00 / 0x006A4480 / 0x0069FD60 / 0x006A8D20 /
  0x006A8C88 / 0x006A8DE0; none of these sites is used here.
- Not part of any profile part ([README](README.md#compatibility-and-interactions)). No Developer card calls its
  `RenderDeveloperUI`, so its status line (below) is not shown in the current menu.

## Limitations

- Native priority differences still apply within the boosted floors; other lots and rooms with zero native priority are
  unchanged.
- The extra solves run only on the render thread and only for the priority lot.

## Technical reference

### The game side (Steam 1.67.2)

- **One room at a time for the whole world.** The scheduler 0x006C5C20 (fastcall(tree = lightMgr+0xD4), plain `ret`),
  reached by the tail `jmp` at 0x006C5E39 of the per-frame light tree update 0x006C5E20 (after the rooms' gathers),
  returns at once while `[tree+0x74]` (the current room) is set. Otherwise it asks 0x006A8190 for (room, priority) of
  every room of every level of every lot, sorts them, and makes the best one current (above 0.001; 0x0069E860: state 3,
  the manager's polling set). The current room is cleared only by FinalizePrime (0x006A0E00 -> 0x006C4870) or an
  invalidate. The lot pass (0x00ADB8F0 -> 0x006A8BA0 -> 0x006A88B0 -> 0x006A3F80) solves it within its lot's budget
  (5 / 10 / 15 / 30 ms), and the next room is picked only on the next frame.
- **The priority** (0x0069E770, fastcall(room), float in ST0; its only call is 0x006A81DF): 0 unless state 2. Class 0:
  10000 x {1 camera story or outdoor below, 0.8 indoor below, 0.5 above} x (0.5 on a lot not in high quality); class 1:
  1000; class 2: 100; 0 when the class is above LodChoice. So every pending first solve of every loaded lot goes before
  any upgrade of the viewed story.
- **The LOD ladder** (0x0069EA70, at the end of every solve): class 0 -> 1 -> 2, one full solve per step (wall rows 4 / 7
  / 13). An invalidate (0x0069EED0, 0x0069F160) restarts at class 0 unless the room was solved before (`+0x100 != 4`)
  and its shown class is at least LodChoice (the `jl` at 0x0069EF58 / 0x0069F1C5).
- An invalidate of the room being solved throws its work away (0x006C4870, then 0x0069E950(0): no commit).
- The game's own solve time per class is kept at 0x011D1200 / 04 / 08 (ms, added by 0x006C2380).

### The patch

Every write goes through `MemPatch::WriteCodeSuspended`, and Stop puts the bytes back.

1. **Viewed lot first:** the CALL at 0x006A81DF -> `PriorityHook`. The factor comes from
   `RoomAmbientPolicy::FloorPriorityFactor`: 4000 on the camera's story, 2000 below it, 1 above it; 4000 on every floor
   while `LevelLightShare::AllFloorsDetailed()`; 1 for other lots. Zero priorities are unchanged. The priority lot is the
   one the game gives 15 ms (0x006FDE10 SceneObjectManager, 0x006FDC80 against its +0x10D0 / +0x10E0), with the story
   manager's lot id `mgr+0x90` / `+0x94`; the camera story is `mgr+0x284`, the room's level `mgr+0x88`.
2. **No middle step:** 0x0069EAA2 `BF 01 00 00 00 8D 5F 01` -> `8B F8 BB 02 00 00 00 90` (mov edi,eax; mov ebx,2).
3. **Requeues keep the class:** the two `jl` (`7C 0B`, `7C 02`) -> `90 90`.
4. **Several rooms per frame:** the `jmp` at 0x006C5E39 -> `PickHook`. It runs the scheduler, then, only on the render
   thread (id taken at Present) and only when the pick made a new room current that belongs to the priority lot (story
   built, `mgr+0x280`), solves the room at once (0x006A3F80 with a game stopwatch of its own: 0x004F35B0 kind 4 = ms,
   0x00408700, 0x004F33C0) and picks the next, until 4 ms (1 ms while `LotLightingMotion::SampleCameraMoving()`) or a
   room that did not finish (the lot pass goes on with it next frame). The drain runs only when the room current at the
   previous pick is done and the new one is of that same lot (so that lot's pass ran: not paused by +0x18 / +0x4E).
5. **Stranded rooms:** a room in state 2 (`room+0xF0`) with a pending class above its LodChoice (`room+0xF4 > 0`) gets
   priority 1 instead of 0.

### Apex's own requeues

In [`features/level_light_share.cpp`](../../../features/level_light_share.cpp):

- `QueueRoom` never invalidates the room being solved (state 3); it is sent again when that solve is over
  (`FlushDeferred`, from the room update).
- It holds back only the requeues after a setting or ambient change (`defer`), never a requeue caused by a lamp list
  change (its list may point to a lamp being deleted), and only while the room update can send it later. A whole-world
  relight asked while the story share is off runs at once.
- Whole-world relights asked in a burst run once, 250 ms after the last ask; the settle requeue is armed once per lot
  state.
- The per-point hooks return at once for a light the game is about to drop (colour sum under room+0x63C, the game's own
  sums in the same order).
- Night Lighting does not relight every lot 3 s after a world loads at night (the game has just solved them).

### Status line

"viewed lot first on (N of M priorities raised), no middle step, requeues keep the class, several rooms per frame
(frames, extra solves, finished, ms)" and the game's own solve time per class.

### Source

`features/room_light_queue.{h,cpp}`: `Start`, `Stop`, `Running`, `PriorityHook`, `Factor`, `Stranded`, `PickHook`,
`DrainableRoom`, `StatusText`, `RenderDeveloperUI`. Group `RoomLightQueue`.

## Rejected approaches

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-room-light-queue.md)
- [History](../../history/performance-room-light-queue.md)
- [Room light maps](../../engine/room-light-maps.md)

# Room to Save

The Sims 3 is a 32-bit game: in a long session a save can fail with Error 12 even though plenty of memory is free in
total, because no single free block is large enough for the save. Room to Save keeps a block of free address space in
reserve and hands it back right before every save, and when memory gets tight it empties the game's cache of files it
is not using. Nothing on screen changes.

## Status

| | |
|---|---|
| Availability | Released in 2.7.0 (Experimental until 2.15.0). Earlier buffer release during saving and compatibility with sims3-modernization: Released in 2.14.0. The one-time question card after an update or first install: Released in 2.17.0 |
| Default | On |
| Menu | System > Performance > Memory handling > *Room to save* |
| Configuration | `[patches.MemoryGuard]` in `ApexRadiance.toml` |
| Source | [`features/memory_guard.{h,cpp}`](../../../features/memory_guard.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) (`MemoryGuardPatch`) |

## The problem

The large-address-aware 32-bit executable has up to 4 GiB of user address space on 64-bit Windows. A long session can
exhaust that space or fragment it so a large allocation fails despite free bytes elsewhere. The native world save
(0x00C6D460) returns a success flag; its caller maps false to result 12. This return path does not identify the failed
allocation or prove that every Error 12 has the same cause. The game also retains idle resource files, which its own
cache trim can release.

## How Apex Radiance solves it

1. **Keep a reserve.** At start, a 128 MB block of address space is reserved (`MEM_RESERVE` only, top-down: no memory
   is committed).
2. **Hand it back before a save.** The world save's call (0x00AAC320) goes through Apex: the reserve is released and,
   when the save runs on the thread of the resource system's update, the game's own shrink of its two resource caches
   runs first (idle entries only). Then the game saves.
3. **Watch the largest free block** every 4 s. Under 320 MB the resource caches are shrunk (at most once a minute, on
   the thread of their per-frame update); under 160 MB the reserve is released too. Once a free block of 640 MB is back
   (the reserve plus 512 MB) and 30 s have passed, the reserve is taken again.

Only idle resources are dropped; the game reads them again when it needs them. The reserve cannot be retaken while
the native save hook is active.

Every build adds three steps (`kMemoryOptimizationsEnabled` in [`build_flavor.h`](../../../build_flavor.h); details in
[Bounded memory handling](../memory-budget.md)):

- **Partial idle-cache steps.** After three stressed scans in a row (largest free block under 320 MB, under 512 MB
  free in total, a recent failed allocation, or private memory of 2 GiB with little room in the lower 2 GiB), the
  resource system's update asks each cache to drop up to 16 MiB of idle entries, keeping 64 MiB idle. Steps are at
  least 120 s apart; one that frees under 1 MiB or takes over 8 ms waits 10 minutes. Steam 1.67.2 only, after the
  native trim at `0x00732E90` and its call are checked; never during a save.
- **Save-phase buffer cleanup.** Before the world save, queued block releases of *Faster memory handling* are drained
  and inactive Apex compression buffers are given back ([save preparation](../memory-budget.md#save-preparation)).
- **Early save preparation.** On Steam 1.67.2 the save dispatch call (`0x00AB12AF` to `0x00AAC110`) also goes through
  Apex: the same buffer cleanup, the game allocator's own trim
  ([fast-memory.md](fast-memory.md#allocator-trim-and-top-down-large-blocks)) and the release of the reserve run
  before the dispatch, ahead of the world-save hook.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Room to save | `[patches.MemoryGuard] enabled` | bool | on | | Turns the reserve and the cache shrink on. A missing key reads as on |

The row is part of the Overview's Performance group switch. It is not in the Performance part of profiles: applying a
profile leaves it as it is.

Once per installation, the first time the menu is available, a question explains the feature in plain words and asks
whether to keep it on (**Keep it on** / **Turn it off**); the answer sets the switch and `[ui] room_to_save_asked`
([ui.md](../../ui.md#startup-and-notices)).

## Compatibility and interactions

- **Faster memory handling** ([fast-memory.md](fast-memory.md)) changes the allocator lock and big-block releases; the
  two work together.
- **Lot Streaming** ([lot-streaming.md](lot-streaming.md)) with more detailed lots uses more address space.
- **Vulkan driver guard** ([vulkan-driver-guard.md](vulkan-driver-guard.md)) keeps an unused driver out of the same
  address space.
- **sims3-modernization:** its `ts3native.asi` can detour the save dispatch entry `0x00AAC110` with a `jmp rel32`.
  The early save preparation accepts that entry only when the jump lands inside a loaded `ts3native.asi`; the call at
  `0x00AB12AF` is still verified byte for byte, and Apex's wrapper calls the entry as before, so the save passes through
  both. The log says *The world save's entry is ts3native's detour (sims3-modernization); the early save preparation
  goes in front of it*.
- **Game build:** Steam 1.67.2 with fixed addresses and byte checks; other builds (the EA app's 1.69) through signatures
  of the same five sites, checking that the calls reach their targets.

## Limitations

- It cannot create memory: a save that needs more than the reserve plus what is free still fails.
- The resource caches are only shrunk from the thread of their own update; a save made on another thread skips the
  shrink and only releases the reserve.
- The idle-cache size in the log (`+0x98` of each cache) is native accounting, not physical RAM returned to Windows.
- The hook begins at the native world save; earlier serialization and total save peak remain unmeasured.

## Technical reference

| Site (Steam 1.67.2) | What | Patch |
|---|---|---|
| 0x007377F7 (in 0x007377F0) | The ResourceSystem's per-frame update call to 0x00737560 | CALL redirected to `UpdateHook`: runs the update, remembers the system and its thread, shrinks when asked |
| 0x00733E70 | ResourceSystem vtable 0x00FFE2F0 slot +0x50: `Trim` on both caches (+0x1E0, +0x1E4), every idle entry | Called by Apex |
| 0x00AAC320 (after `mov ecx,[esp+30h]` at 0x00AAC31C) | The world save call to 0x00C6D460 (thiscall, 2 arguments, `ret 8`, `al` = saved) | CALL redirected to `WorldSaveHook` |

Every call site is checked before it is rewritten and written back when the feature is turned off. A helper thread
(`Watch`) samples the largest free block with `VirtualQuery`. Address group `MemoryGuard` (`ResUpdateCall`,
`ResUpdate`, `ResShrinkBoth`, `WorldSaveCall`, `WorldSave`).

Log lines: `[MemoryGuard] On: a reserve of 128 MB ...`, `[MemoryGuard] World save done / FAILED (Error 12): largest free
block N MB, M MB for the save (...)`, `[MemoryGuard] Largest free block N MB: the reserve of 128 MB let go` and
`... the game's resource cache emptied (N MB of idle files)`. `MemoryGuard::StatusText` (largest free block and its
lowest value, reserve held or let go, world saves and failures, cache shrinks and the idle megabytes they freed) and
`RenderDeveloperUI` (with a button *Empty the game's resource cache now*) exist, but no Developer card calls them in the
current menu.

## Rejected approaches

- Accepting any `jmp` at the save dispatch entry: only a jump into a loaded `ts3native.asi` is a known detour
  ([history](../../history/performance-room-to-save.md)).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-room-to-save.md)
- [History](../../history/performance-room-to-save.md)

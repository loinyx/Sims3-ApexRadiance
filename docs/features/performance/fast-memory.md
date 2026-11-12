# Faster Memory Handling

Less waiting when the game hands out and frees memory. Every part of the game shares one memory manager. When two parts
need it at once, the second one goes to sleep almost immediately and wakes up late, and freeing a big block of memory
makes everyone wait. Faster Memory Handling lets a waiting thread spin a few microseconds before it sleeps, and hands
big blocks back to Windows in the background. Nothing the game allocates changes.

## Status

| | |
|---|---|
| Availability | Released in 2.4.0; on by default since 2.5.5. Allocator trim and top-down large blocks: Released in 2.14.0 |
| Default | On |
| Menu | System > Performance > Memory handling > *Faster memory handling* |
| Configuration | `[patches.FastMemory]` in `ApexRadiance.toml` |
| Source | [`features/fast_memory.{h,cpp}`](../../../features/fast_memory.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game uses one EA PPMalloc general allocator (dlmalloc style) for every thread, protected by one critical section.
The game creates that critical section with a spin count of 10, which Windows turns into about 24 ns of spinning
(effectively none); the game's other critical sections get the default 2000 (about 5 us). So a thread that finds the
allocator busy goes to sleep at once. In addition, blocks of 128 KB and more are released with `VirtualFree` while the
allocator lock is held, so every other allocating thread waits for that system call.

## How Apex Radiance solves it

1. **Spin count.** `SetCriticalSectionSpinCount(cs, 2000)` on the allocator's critical section (Windows keeps the flag
   bits). Put back on Stop.
2. **Background release.** The big-block release call is redirected to a stub that queues the address and wakes a
   helper thread, which releases it outside the lock.
3. **Allocation safety.** The allocator's five `VirtualAlloc` calls go through Apex: if one fails while releases are
   queued, the queue is released at once and the call is made again, so a queued release can never make an allocation
   fail.

The allocator's state and every value it reads are the same; the spin only changes how long a waiting thread spins
before it sleeps, and the address range of a freed big block goes back to Windows microseconds later.

### Allocator trim and top-down large blocks

Two further changes to the same allocator have no setting and are on in every game session on Steam 1.67.2
(`features/allocator_tuning.cpp`), independently of *Faster memory handling*:

- **Trim.** Every 60 s and before every world save, Apex calls the allocator's own trim (`0x004E4F90`, thiscall
  `(allocator, pad)`), which decommits the free tops of its cores while keeping a 1 MB pad each (the allocator's own
  pad is 64 KB), so fewer pages are committed again soon after. The trim before a save runs from the early save
  preparation of [Room to save](room-to-save.md#how-apex-radiance-solves-it) (`SaveDispatchHook`), so it needs *Room to
  save* on and that hook installed; with *Room to save* off only the 60 s trim runs.
- **Top-down large blocks.** The allocator's flag at `+0x498` is set to 1, so its large blocks are reserved from the top
  of the address space, away from the smaller allocations at the bottom.

Both require the Steam image base `0x00400000` and the trim's prologue bytes `83 EC 10 53 55 56 8B E9`; otherwise the
log says *Not the Steam 1.67.2 allocator: off*. The log lists the first trims and any trim that freed 1 MB or more.

### Address-space experiments (not shipped)

Three development-build switches, off unless `[dev_build]` sets them to `true`, Steam 1.67.2 only. They come from
sims3-modernization's `[BigBlock]` and `[StreamBuf]` research and have not been measured with Apex Radiance yet.

- **`mono_cores_bottom`** (needs *Faster memory handling* and `allocator_top_down`). The Mono sub-heap takes its 1 MB+
  cores from the game allocator as large blocks, so top-down placement sends these long-lived blocks among the
  short-lived ones. The call in the Mono core supplier (`0x00D71553`, rel32 at `0x00D71554`, window `0x00D7154A`
  checked) goes to `MonoCoreMalloc`, which marks its thread (one slot) around the game's Malloc; the large-block
  `VirtualAlloc` stub (return `0x004E5130`) drops `MEM_TOP_DOWN` for that thread.
- **`big_block_reuse`** (needs *Faster memory handling*). The helper thread decommits a released large block of 64 KB to
  16 MB and keeps its reservation (at most 128 MB, 256 blocks; the oldest released first). The next large block of the
  same 64 KB size class is committed in it (zero pages, as a new block). Every kept reservation is released before a
  save (`PrepareForSave`) and when an allocator `VirtualAlloc` fails.
- **`stream_arena`** ([`features/stream_arena.{h,cpp}`](../../../features/stream_arena.cpp)). `EA::IO::MemoryStream`
  never grows in place: each size change is a new buffer, a copy and a free, and every buffer of 128 KB or more was its
  own `VirtualAlloc`. Buffers of 128 KB to 8 MB come from one 48 MB reservation (top-down) in 64 KB slices, committed on
  use, zero-filled when reused and decommitted 5 s after their last use. Sites: the SharedPointer constructor's
  allocator call `0x004DEC0B` and the two data frees `0x004DEA99`, `0x004DEEF9` (their three functions checked byte for
  byte, written while no thread is stopped inside); a pointer outside the arena goes to the game's free. Before a save
  the free slices are decommitted; when *Room to save* lets its reserve go, no new arena block is made and the
  reservation is released once none is in use.

The save path logs `[FastMemory] Before the save: ...` and `[StreamArena] Before the save: ...` with their counters.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster memory handling | `[patches.FastMemory] enabled` | bool | on | | Applies the spin count and the background release. A missing key reads as on |

## Compatibility and interactions

- The import slots are found in TS3W's import table and called through, so another module's IAT hook on
  `VirtualAlloc` / `VirtualFree` is kept.
- Not part of any profile part ([README](README.md#compatibility-and-interactions)). No Developer card; the Frame
  Profiler report carries "Faster memory handling: ..." (the spin 10 -> 2000, the releases done by the helper and any
  `VirtualAlloc` retry).
- [Vulkan driver guard](vulkan-driver-guard.md) and the [address space monitor](../frame-profiler.md#address-space)
  address the same 32-bit address-space limits from other angles.

## Limitations

- The core tail decommit (0x004E504C) and the core release (0x004E48AB) are never deferred.
- When the queue is full (32 entries), when stopping, or for anything other than a plain release, `VirtualFree` runs on
  the calling thread as before.
- If exactly five `VirtualAlloc` calls are not found next to the release call, the feature stays off.
- The allocator trim and top-down blocks are not measured in game. In one development session every trim freed 0 MB,
  because the game already returns the tops of its cores when it frees them.
- The allocator trim and top-down blocks cannot be switched off from the menu.

## Technical reference

### The game side (Steam 1.67.2)

- Global allocator pointer 0x011CB864 (operator new 0x004E3F90 reads it). One CRITICAL_SECTION at allocator+0x4E8
  (pointer at +0x4E4), taken by every Malloc / Free, created by InitializeCriticalSectionAndSpinCount(cs, 10) at
  0x004E48D1. Windows' spin budget is SpinCount x 10 TSC ticks.
- Blocks of 128 KB and more (`[+0x494]`) get their own `VirtualAlloc` (0x004E512A) and are freed with
  `VirtualFree(base, 0, MEM_RELEASE)` at 0x004E5306 inside FreeInternal, lock held, result ignored.
- Other `VirtualAlloc` calls: new core 0x004E4E81 / 0x004E4EB4 / 0x004E4EDB, core growth 0x004E5536.

### The patch

- The spin count is set after checking `[a+0x4E4] == a+0x4E8`.
- The release call (6 bytes, `FF 15 [VirtualFree slot]`) becomes `nop; call Stub_Free`. The CALL ends where the original
  did, so a thread still inside the stub when the bytes are written back returns to an instruction boundary. The stub
  queues the address (32 entries, SRW lock) and wakes a helper thread (above-normal priority) that releases the queue.
- The five `VirtualAlloc` calls are found at start as `FF 15 [VirtualAlloc slot]` within [release − 0x600, release +
  0x300) (exactly 5, or the feature stays off) and become `nop; call Stub_Alloc`: the same call; on failure the queue is
  released on the spot and the call is made once more.
- Every write is `MemPatch::WriteCodeSuspended`. Stop restores the release first, empties the queue, then restores the
  allocation calls.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| AllocGlobal | 0x011CB864 | Sig (dword in the operator new pattern) |
| AllocMmapFreeCall | 0x004E5306 | Sig |

Group `FastMemory`.

### Measuring

A/B with the Frame Profiler's sampling run: the "system code called from" rows 004E5935 / 004E5954 / 004E69E5 /
004E6A0B (allocator lock) and 004E530C (the release).

## Rejected approaches

- Returning the free pages inside the allocator's cores to Windows: splits and coalesces write headers into the free
  range, so every path that hands out a chunk would need a recommit ([history](../../history/performance-fast-memory.md)).
- Returning the free pages of the script heap and of the allocator's cores to Windows, committed again by the internal
  malloc and realloc and by a fault handler (development builds, 2026-10-27): it worked in game (120 to 245 MB of private
  memory given back, no fault) but leaves the address space as it is, which is what runs out in a 32-bit session; the
  measure players watch (address space in use) did not change ([history](../../history/performance-fast-memory.md)).

The trim freed nothing in one development session; it is kept until measured in game
([history](../../history/performance-fast-memory.md)).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-fast-memory.md)
- [History](../../history/performance-fast-memory.md)

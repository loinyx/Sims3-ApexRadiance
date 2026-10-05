# Faster Memory Handling

Less waiting when the game hands out and frees memory. Every part of the game shares one memory manager. When two parts
need it at once, the second one goes to sleep almost immediately and wakes up late, and freeing a big block of memory
makes everyone wait. Faster Memory Handling lets a waiting thread spin a few microseconds before it sleeps, and hands
big blocks back to Windows in the background. Nothing the game allocates changes.

## Status

| | |
|---|---|
| Availability | Released in 2.4.0; on by default since 2.5.5 |
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

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-fast-memory.md)
- [History](../../history/performance-fast-memory.md)

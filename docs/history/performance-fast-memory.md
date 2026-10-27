# Faster Memory Handling: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/fast-memory.md](../features/performance/fast-memory.md).

### 2026-09-30: allocator lock and big-block release

**Context:** memory research (session notes `loadre\memory.md`), checked against `full.asm` and the development PC's
SysWOW64 `ntdll.dll`.

**Finding:** every thread uses one EA PPMalloc allocator behind one critical section created with a spin count of 10
(about 24 ns, effectively no spin) where the game's other critical sections get the default 2000 (about 5 us). Blocks of
128 KB and more are released with `VirtualFree` inside the allocator's free path while the lock is held.

**Outcome:** commit `6e34709`: spin count 2000 and the big-block releases moved to a helper thread, with every
allocation call retried after draining the queue when it fails.

### 2026-09-30: retry and teardown fixes

**Outcome:** commit `aab1121`: the allocation retry waits for a drain in progress; the helper's events are kept; no
thread-exit wait under the loader lock.

### 2026-10-20: allocator trim and top-down large blocks (2.14.0 development builds)

**Context:** development switches `allocator_trim` (the allocator's own trim every 60 s and before saves, 1 MB pad) and
`allocator_top_down` (allocator `+0x498` = 1) were added to test address-space fragmentation.

**Finding:** in one development session every trim freed 0 MB: the game already returns the tops of its cores on free.
A crash during a later session was at `TS3W+0xAD6A74` (`movaps xmm0,[esi]`), a stack overflow of the game's own thread
in a function that scales a buffer of floats, called from `TS3W+0xAEC795`, a per-channel loop with large stack buffers
in the game's audio decoder. No Apex address was on that thread's stack, and it was the first TS3W crash in the event
logs of the previous 20 days.

**Outcome:** no change. Both switches are on in 2.14.0 with no setting. If the crash repeats, the first test is a
development build with `allocator_top_down` and `allocator_trim` off.

### 2026-10-27: memory map of a 2.16.2 session and the free memory inside the game's domains

**Context:** a read-only map of a running game (address space walk, the allocator's core ring and chunks, the script
collector's sections, thread stacks, the Windows heap) to find large reductions that leave the lighting as it is
([engine page](../engine/native-allocation-and-script-memory.md#domains-measured-in-game)).

**Finding:** of about 2.4 GB of private memory, the general allocator held 906 MB in 57 cores with 164 MB free inside
them, the script heap 368 MB with 101 MB of free blocks, the Windows heap about 300 MB, the graphics driver's own heaps
and write-combined mappings about 350 MB, and Apex Radiance about 60 MB (image, C++ heap, high buffers). The free space
inside the allocator's cores grew from 46 MB after a load to 164 MB later in the session, mostly in eleven high cores
that a few live chunks keep from being released.

**Outcome:** no decommit of free memory inside the game's domains. In the allocator, a split or a coalesce writes chunk
headers into the free range and kernel-mode reads into a freshly returned buffer would fail on a decommitted page, so
every path that hands out or splits a chunk would need a recommit hook. The script collector has no decommit path at
all; its block allocator would need a recommit at `0x00E4971D` and the collector's thread ownership is not established.
The measurement of both domains was added to the memory analysis instead, so a later change can be judged on in-game
numbers.

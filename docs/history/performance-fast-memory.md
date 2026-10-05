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

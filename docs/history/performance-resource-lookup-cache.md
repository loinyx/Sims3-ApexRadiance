# Faster Game File Lookups: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/resource-lookup-cache.md](../features/performance/resource-lookup-cache.md).

### 2026-09-29: lookup cache (plan candidate C1)

**Context:** resource lookups dominated about 65% of the small hitches ([group history](performance.md)). The plan
(section 2.2) measured the threads that call FindProvider: render, loader workers, simulation.

**Finding:** FindProvider is reached only through two vtable slots and the wrapper slot; the read-only package class can
gain keys only when its file changes, which the engine reports through DatabaseChanged. Detouring the entry was
rejected: the slots are its only references, the Frame Profiler wraps the same function, and changed code bytes would be
seen by the game-address self-check. `framework/slot_chain.h` was written so the profiler and the cache share the
slots.

**Outcome:** shipped experimental, off by default, with developer checks (1 answer in 64, every answer for 10 s).

### 2026-09-29: reliable read-only packages (round 3)

**Context:** the transient open failure risk: a read-only package without its key set that fails to open answers "no".

**Finding:** after such a failure the package is closed with no key set; a successful probe leaves it open.

**Outcome:** `Remember` refuses to store when a read-only package above the answer is in that state
(`ReadOnlyAboveReliable`), for found answers too.

### 2026-10-02: on by default (2.5.5)

**Outcome:** default changed to on, with the other switches. See [group history](performance.md).

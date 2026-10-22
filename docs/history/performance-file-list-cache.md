# Faster File Lists: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/file-list-cache.md](../features/performance/file-list-cache.md).

### 2026-09-29: GetKeyList cache (round 3, section 4.2)

**Context:** CAS LoadBlendGeometryCallback 0x005DA0C0 asks for every key of type 0x0A037DDA and every one of about 300
packages walks its whole index. This was 17% of the SimService-dominated hitch samples and 5.2% of all hitch samples in
the measured session.

**Finding:** the read-only class already returns a package's keys in two different orders (key set or open index), and
the known callers sort and unique.

**Outcome:** shipped experimental, off by default, with a set comparison as its developer check.

### 2026-10-02: on by default (2.5.5)

**Outcome:** default changed to on. See [group history](performance.md).

### 2026-10-22: shared key lists

**Context:** commit `f58a6f0`. A cached GetKeyList call copied each package's kept list under the lock: about 207
packages a call, one allocation each, 2 ms a call on average and up to 4 ms on the render thread.

**Outcome:** the kept lists are immutable vectors shared through `std::shared_ptr`; a lookup takes a reference and
appends the same keys to the game's vector. Answers are unchanged.

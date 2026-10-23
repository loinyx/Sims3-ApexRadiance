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

### 2026-10-22: writable packages' lists by their write epoch

**Context:** commit `000f3bc`. In a CPU sample the GetKeyList walks of the 22 writable packages, asked on every call,
were 1.2% of the render thread.

**Finding:** the write epochs of Remember missing files already hook every writer of the index of three writable
classes (DPF, DDF, packed stream). While a package's epoch is unchanged and no counted write is in progress, its list
is what its index held when it was captured, from the same walk, so in the same order.

**Outcome:** those packages' lists are kept with their write epoch and answered while it is the same, stored only from
a call during which no counted write happened. Behind the Tester switch *Writable packages' file lists*
(`perf_key_list_counted_packages`, on); gameplay checks are open
([validation](../validation/performance-file-list-cache.md)).

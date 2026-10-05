# Remember Missing Files

An extension of [Faster Game File Lookups](resource-lookup-cache.md). About a third of the game's file lookups ask for
files that no package has, and each of those searches every package. Remember Missing Files keeps those "not found"
answers too, and counts the writes of the packages that can change, so most remembered answers need no re-check at all.
What the game loads is unchanged.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier; on by default since 2.5.5 |
| Default | On |
| Menu | System > Performance > Files and objects > *Remember missing files* (shown under *Faster game file lookups*) |
| Configuration | `[patches.ResourceLookupMisses]` in `ApexRadiance.toml` |
| Source | [`features/resource_cache.{h,cpp}`](../../../features/resource_cache.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The resource resolve 0x007D8110 looks a key up and, on a miss, looks up the variant 0x007D7580 builds (group ^
0x08000000, and for five types with instance-high 0 also instance ^ 0x08000000). With the lookup cache on, about 36% of
FindProvider calls are keys that exist in no package. Each costs a full scan (about 19 us), and together they are most of
the remaining lookup time. Cache hits also still probe every non-read-only package above the answer (about 17 packages,
about 1.3 us per hit).

## How Apex Radiance solves it

1. **Absent entries.** The game's 0 answer is stored like a found one: provider 0, index "all" (every non-read-only
   package counts as above it), under the same generation rules. On an answer from memory the cache returns 0 and
   leaves `*priorityOut` alone, as the game does.
2. **Re-check** by probing every non-read-only package. Any "yes": the game's lookup runs and replaces the entry.
3. **Write epochs.** For the database classes whose write paths were traced, every write is counted. An entry stores
   the sum of the epochs of its counted databases; while the sum is unchanged those databases need no probe.

Read-only packages cannot gain keys without RegisterDatabase or DatabaseChanged (which bump the generation); every other
package is either probed or has its writes counted.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Remember missing files | `[patches.ResourceLookupMisses] enabled` | bool | on | | Keeps "not found" answers and installs the write-epoch hooks. Idle ("Waiting: needs Faster game file lookups") while the lookup cache is off. A missing key reads as on |

Verification uses the lookup cache's controls ([resource-lookup-cache.md](resource-lookup-cache.md)).

## Compatibility and interactions

- Needs [Faster game file lookups](resource-lookup-cache.md). The Overview group switch enables the lookup cache first
  and stops this extension first.
- Switching this feature, or the lookup cache, on or off bumps the generation: nothing stored under the other mode is
  used.
- The epoch hooks are installed only while both the lookup cache and this switch are on.
- **Frame Profiler:** hitch lines add ", absent from cache N"; the report adds "Remember missing files: ...".
- **Official Sims3SettingsSetter:** its source has none of the database vtables, their methods or 0x004A7FC0.

## Limitations

- Write epochs cover only the traced classes and paths. A write path missed by the study would leave a counted
  database's answers stale until the next generation or 60 s. The developer checks compare with the game; any
  difference turns the cache off. Classes are all-or-nothing: a changed method keeps its class probed.
- Writes to a database that shares an epoch bucket with another re-check more entries (never fewer).
- A record open on a loose-file folder (DDF) bumps its epoch (a record open can insert a newly appeared file), so a
  mods folder with many loose files read often keeps re-checking the entries below it (probes, not game lookups).
- MemoryDB and the ContentManager database are always probed.

## Technical reference

### The read-only exception

A read-only package with no key set (`[db+0xB0] == 0`) whose file cannot be opened at that moment answers "no" for every
key (0x007345D0 / 0x00734710). After the game's lookup, such a package is closed with no key set (`[db+0xB0] == 0 &&
[db+0x14] == 0`; a successful probe leaves it open, and the idle close builds the key set before closing). `Remember`
checks every read-only package above the answer (all of them for "absent") and stores nothing when one is in that
state (counter "not remembered (a read-only package could not answer for sure)").

An absent answer also needs every read-only package of the list to have been reliable before the game's lookup
(`roBefore`, computed in `Hook_FindProvider` only while this switch is on). Reliability only goes from unreliable
(closed, no key set) to reliable (open, or key set built), so a package that was unreliable when the probe started keeps
the answer out. Residual (inferred): a package that was reliable before only through its open file, was closed as idle
during the probe, failed to reopen for it, and was reopened by another thread before the check after it. The developer
checks would log it.

### Write epochs

The probe is OpenRecord(key, 0, 1, 6, 1, 0). For each non-read-only class, every code path that can change its answer
was traced in the disassembly:

| Class (vtable) | What the probe reads | Writers hooked (slot: function) | Bump when |
|---|---|---|---|
| DPF, writable package (0x00FB2600; derived 0x01048DA0 overrides only +0x7C / +0x84) | Hash index `[this+0x2D0]` (vfunc +0x28) under the mutex +0x270; closed: auto-open 0x004A6860 -> slot +0x18 | +0x00 dtor 0x004A8D00 / 0x00996630, +0x08 shutdown 0x004A6AE0, +0x18 open 0x004A76E0, +0x1C close 0x004A8D20, +0x24 flush / compaction 0x004A9B70, +0x34 OpenRecord 0x004A94C0, +0x3C CloseRecord 0x004A8E00, +0x40 DeleteRecord 0x004A85A0, +0x5C set index 0x004A6690, +0x8C load index 0x004A9950, +0x9C convert index 0x004A6C80; entry of the non-virtual direct write 0x004A7FC0 (callers 0x007D7A50 / 0x007D7B30 / 0x007D7C10, KeyList copies; 0x004AE510, compaction temp) | Always, except OpenRecord (only a record asked with write access or a disposition other than 6 / 3: insert or replace) and CloseRecord (only `[rec+8] == 0x12E4A892`, a writable record: commit 0x004A8910 removes and re-inserts the key, and leaves it removed on failure) |
| DDF, loose-file folder (0x00FB2420) | Map `[this+0x50]` under the mutex +0x80 when `[this+0x0C]` | +0x00 dtor 0x004A6030, +0x08 0x004A3950, +0x18 open 0x004A3AD0 (full rescan), +0x1C close 0x004A5240, +0x2C set location 0x004A4370, +0x34 OpenRecord 0x004A62B0, +0x40 DeleteRecord 0x004A52A0, +0x58 one-file refresh 0x004A6100, +0x5C rescan 0x004A5340 (both also from the directory watcher thread, through the vtable; DatabaseChanged comes after the change) | Always, except OpenRecord: whenever a record is asked (it inserts a key whose file appeared on disk even for reading) |
| Packed stream (0x00FFD790; not the read-only class) | Open mode `[this+0x14]` and index `[this+0x70]` | +0x00 dtor 0x0072CD30, +0x08 0x0072C6A0, +0x18 open 0x0072D790, +0x1C close 0x0072C8F0, +0x2C set location 0x0072CD50 (it cannot create or delete records) | Always |
| MemoryDB (0x00FFD5F8) | Not counted: the non-virtual PutResource 0x0072C350 (callers 0x007D7CF0 / 0x007D7DD0 / 0x007D7EB0, 0x00D57B90) inserts keys | | Always probed |
| ContentManager (0x01046EE8, downloaded content, priority -1200) | Not counted: it forwards to sub-databases and its maps change in non-virtual install code (0x0098B5E0, 0x009905E0, 0x00989B50, 0x00984B60, 0x00987C20, ...) | | Always probed |

- Every studied function's first bytes are checked before its class is counted. A class with any difference, an
  unreadable slot, or a slot another module changed stays probed (all or nothing per class). The DPF classes also need
  the entry hook of 0x004A7FC0 (`EntryChain` site `DpfWriteDirect`, layer `ResourceCache`, prologue `83 EC 28 53 56`, no
  jump lands inside it). Record objects need no hook: they close through the database's slot +0x3C (0x004AD9F0, dtor
  0x004AE080).
- Counters (`WriteBegin` / `WriteEnd` around each hooked call): `g_writeBusy` (writes in progress), `g_writeSeq` (bumped
  at begin and end) and a per-database epoch in 1,024 buckets by pointer (a shared bucket only costs an extra re-check).
  Nested calls (OpenRecord -> DeleteRecord, flush -> close / open, the probe's own auto-open) count twice. `WriteEnd`
  runs in a `__finally` (`Bracketed`), so an exception unwinding through the game's write never leaves `g_writeBusy`
  raised (which would keep every epoch sum "unknown" until restart).
- An entry stores the sum of the epochs of its counted databases (the non-read-only ones above it, and its own package
  when it is not read-only). The sum is taken only when no write was in progress and none happened since before the
  game's lookup (seqlock on `g_writeSeq`); otherwise it is "unknown".
- **Answer:** sum unchanged (read with no write in progress and the sequence stable around it) -> no probe of the
  counted databases, and none of a read-only answering package (same premise as the cache itself). Databases of
  uncounted classes are still probed. Sum changed or unknown: every package is probed as before; if that passes with no
  write meanwhile, the entry's sum is refreshed (counter "sums refreshed"). A write therefore costs each affected entry
  one probe round, not a game lookup.
- **Grace period:** a write that was already running inside a game function when its slot was swapped is not
  bracketed. Sums are neither taken nor trusted during the first 10 s after the hooks go in (answers are probed as
  before meanwhile); every such call has long returned by then.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| DpfVtable / DpfDerivedVtable / DdfVtable / PackedStreamVtable | 0x00FB2600 / 0x01048DA0 / 0x00FB2420 / 0x00FFD790 | Sig (dword stored by the constructor; DPF alternate: its destructor) |
| DpfWriteDirect | 0x004A7FC0 | Sig (entry) |

### Source

`features/resource_cache.cpp`: `StartMisses` / `StopMisses` / `UpdateMisses`, `kSpecs` + `EpochHook<I, N>` +
`Hook_DpfWriteDirect`, `WriteBegin` / `WriteEnd`, `EnableEpochs` / `DisableEpochs`, `InstallClassHooks`, `EpochSum` /
`StableEpochSum` / `RefreshSum`.

Developer counters: "Missing files (on / off): answered from memory, remembered, not remembered" and "Write epochs:
counted classes ...; answers with no probe of them, sums refreshed, writes counted".

## Rejected approaches

- Hooking MemoryDB's PutResource and the ContentManager install code to count their writes: not done; both stay probed.
  See [history](../../history/performance-remember-missing-files.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-remember-missing-files.md)
- [History](../../history/performance-remember-missing-files.md)

# Faster Game File Lookups

Fewer small stutters when objects, textures and lots load. The game asks its resource manager for every file it loads,
and the manager searches every package in turn. Faster Game File Lookups remembers which package answered each file and
re-checks that answer cheaply instead of searching all packages again. What the game loads is unchanged.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier; on by default since 2.5.5 |
| Default | On |
| Menu | System > Performance > Files and objects > *Faster game file lookups* |
| Configuration | `[patches.ResourceLookupCache]` in `ApexRadiance.toml` |
| Source | [`features/resource_cache.{h,cpp}`](../../../features/resource_cache.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

`ResourceMgr::FindProvider` (0x004AFFC0) walks the registered package list (about 290 entries in a typical game) and asks
each package "do you hold this key?" until one says yes. Materials resolving textures during the scene's pending-node
drain, async-load finalize jobs, CAS and lot loading all go through it, often hundreds of times in one frame. Each lookup
probes about 291 packages and takes about 18 us. A few hundred lookups in one frame cost several milliseconds on the
render thread, which shows up as small hitches (8 to 16 ms frames).

## How Apex Radiance solves it

FindProvider is reached only through vtable slots, so Apex Radiance swaps those slots for a hook that consults a table of
earlier answers:

1. **Look up** the (manager, key) pair. No entry: run the game's lookup and store its answer.
2. **Re-check** a stored answer: it is younger than 60 s, the package list has not moved, the answering package still
   holds the key (one probe, exactly as the game asks), and none of the packages above it that can gain files holds it.
   Read-only packages above it cannot have gained the key without a change notice, which empties the table.
3. **Answer** with the stored package and its priority when every check passes; otherwise run the game's lookup.

Any change to the package list (registration, priority change, the engine's own "these keys changed" notice) empties
the table.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster game file lookups | `[patches.ResourceLookupCache] enabled` | bool | on | | Turns the cache on. A missing key reads as on |
| Developer > Performance > *Check 1 answer in N against the game* | `[developer.controls.resource_cache] verify_every` | int | 64 in developer mode, 0 otherwise | 0 to 1024 | Also runs the game's lookup on 1 answer in N and compares. 0 = never |
| Developer > Performance > *Check every answer for 10 s* | (not saved) | action | | | Compares every answer for 10 s |

## Compatibility and interactions

- **[Remember missing files](remember-missing-files.md)** extends this cache with "no package holds it" answers and
  write counting. It is idle while this switch is off.
- **[Faster file lists](file-list-cache.md)** shares the generation counter and package-list watchers (installed while
  either cache is on).
- **Frame Profiler:** its "Resource lookup" counter is the outer layer of the same slots. It times every call (answers
  from memory included), adds "% from cache" per frame and ", from cache N" to hitch lines (after "misses", so
  `agg.pl`, which splits counters on "; ", still parses them), and counts the packages the cache asked as "packages
  probed" (`ResourceCache::TakeLookupNote`). The report adds "Resource lookup cache: ..." and "Resource lookup cache
  counters: ...".
- **Official Sims3SettingsSetter:** its 51 byte patterns overlap neither FindProvider, the resource manager methods nor
  0x00736A70.
- The resource cache does not touch lighting.

## Limitations

- A read-only package whose file changes on disk without the game's file watcher noticing, or a closed read-only
  package re-targeted with SetLocation (slot +0x2C, only allowed while closed; only seen at registration), would change
  what the game finds without a notice. The 60 s entry age bounds it and the developer checks catch it (inferred risk).
- If a read-only package without its key set fails to open during the game's own lookup (file locked by another
  program), the game answers with a lower package. Apex refuses to store such an answer (see *Reliability* below); only
  a racing successful open by another thread could hide it (inferred).
- Every answer probes all non-read-only packages above it; an answer with more than 32 of them above it is never
  stored. With `writable` Resource.cfg lines or many code-registered databases the gain shrinks. The Developer line
  "N not of the read-only class" shows the count.
- Each distinct key costs one game lookup per minute (the 60 s re-check), about 0.1 ms per second for 5,000 keys.
- On a build where the read-only package class cannot be verified, nothing could be cached, so the switch refuses to
  start.

## Technical reference

### FindProvider (Steam 1.67.2)

`ResourceMgr::FindProvider` 0x004AFFC0, thiscall(key*, int* priorityOut), ret 8:

1. Locks the manager's `EA::Thread::Mutex` at `this+0x48` (0x004E16F0 with the infinite-timeout constant 0x00FB2CD0).
2. Reads `begin = [this+0x30]`, `end = [this+0x34]` once and walks the 8-byte entries `{Database*, int priority}`.
3. Per entry: unlock (0x004E17B0), `db->vfunc+0x34(key, 0, 1, 6, 1, 0)`, relock. The first `true` wins:
   `*priorityOut = entry.priority`, return `db`. None: return 0, `*priorityOut` untouched.

- Reached only through vtable slot +0x40 of the base resource manager vtable 0x00FB2DA0 (slot 0x00FB2DE0) and of the
  derived ResourceSystem vtable 0x00FFE250 (0x00FFE290), and through the wrapper 0x004AFDA0 (slot +0x44, calls +0x40).
  There is no direct CALL.
- The "cookie" out-parameter is the winning package's priority (the list's second dword), so the result (package,
  priority) is fully decided by which list entry answers first.
- `db->vfunc+0x34` is the database's OpenRecord(key, record**, access, disposition, flag, info*). With no record and no
  info asked (arguments 2 and 6 = 0) it only answers "do you hold this key?".

### The package list

The only code that writes `[mgr+0x30..0x38]` (its vector helpers 0x004B1F50, 0x004B25B0, 0x004B0B90 have no other
callers):

| Address | Slot | What | Convention |
|---|---|---|---|
| 0x004B2D00 | +0x34 of 0x00FB2DA0 (0x00FB2DD4) | RegisterDatabase(bool add, db*, int priority). Add = insert before the first entry of lower priority (list sorted highest first, ties in registration order), after IsRegistered (+0x38, 0x004AFF70) refuses duplicates; then `db->vfunc+0x48(1, mgr, 1)` (Attach). Remove = Attach(0, ...) then erase. AddRef / Release through `db+4` | thiscall, ret 0xC, returns bool |
| 0x00736A70 | +0x34 of 0x00FFE250 (0x00FFE284) | ResourceSystem's override (its own name map at +0x2A0 and others); calls 0x004B2D00 directly (0x00736C69) | same |
| 0x004B2EC0 | +0x3C of both (0x00FB2DDC, 0x00FFE28C) | SetDatabasePriority(db*, int): erase and sorted re-insert | thiscall, ret 8 |
| 0x004B35A0 / 0x007366A0 | +0 | Destructors (0x004B30B0 frees the vector), game exit | |
| 0x004B0960 | +0x4C of both (0x00FB2DEC, 0x00FFE29C) | DatabaseChanged(db*, keyVector*): the engine's own "these keys of db changed" notice. Runs FindProvider (+0x44) per key and calls the resource cache's listeners (`mgr+0x10` list) for keys whose answer is now `db` or nothing | thiscall, ret 8 |

Other slots: +0x38 IsRegistered, +0x48 0x004B17E0 lists every package holding a key (not a writer), +0x54 0x004B3200
works on a different list at +0xA0 (factories), not the package list.

Who sends DatabaseChanged: the ResourceSystem's file watcher (`ResourceSystem/ShadowWatcher`). Its update 0x00737560
pops changed paths and calls ResourceSystem vfunc+0x14 = 0x00734D10, which for each package object with that path calls
outer+0xC (0x007343C0 "file changed?": size / time against +0x100 / +0xF8, drops the key set) and, if changed,
GetKeyList then `mgr->+0x4C(db, keys)` (0x00734DBF). The loose-file folder databases do the same after a rescan
(0x004A4160).

### Package classes

Every constructor that stores the IDatabase base vtable 0x00FB21F8; all have OpenRecord with `ret 18h`:

| Vtable | Constructor | What | Can gain keys at run time? |
|---|---|---|---|
| 0x00FFE078 | 0x007342F0 ("ResourceSystem/ShadowedDBPF"; inner object at outer+8, outer vtable 0x00FFE0D0) | Every `PackedFile` line of a Resource.cfg without `writable` (0x00737D70 -> 0x00737950): the game's packages, the EP / SP packages, Mods\Overrides / Packages, DCCache .dbc files. Closes idle files and keeps a key set (`this+0xB0`) meanwhile | No, except when its file changes on disk |
| 0x00FFD790 | 0x0072CC60 | Base packed-stream DBPF (read-only OpenRecord, but Open / Close change what it holds) | Treated as yes |
| 0x00FB2600 | 0x004A8F70 ("ResourceMan/DPF") | `PackedFile <path> writable` (opened with access 3) and many world / save / cache callers | Yes |
| 0x00FB2420 | 0x004A5500 ("ResourceMan/DDF") | `DirectoryFiles <folder> [autoupdate]` (loose files) | Yes |
| 0x00FFD5F8 | 0x0072B8F0 ("ResourceSystem/MemoryDB") | In-memory databases | Yes |
| 0x01046EE8 | 0x0098B150 (social cache) | socialCache.package | Yes |

The read-only class in detail: OpenRecord 0x007345D0. With no record / info asked and a key set present it only probes
the key set (0x0072DBD0 -> 0x0072DAD0: hash `key[0] ^ key[3]`, `div` by the bucket count, chain walk 0x005492E0
comparing 16 bytes) under the package mutex (`this+0x40`, 0x0072C6C0). Otherwise AcquireOpen (slot +0x50, 0x00734710:
opens the file with access 1 = read, drops the key set), the base OpenRecord 0x0072D470 (accepts only the open-existing
dispositions 6 and 3: it can never create a record), Release (slot +0x54, 0x007347C0). DeleteRecord (slot +0x40,
0x007346A0) calls 0x00624F70 = `xor al,al; ret 4`. So its key set is the file's index and changes only when the file on
disk changes, which the watcher reports through DatabaseChanged.

The standard Resource.cfg files (Game\Bin, GameData\Shared, GameData\Win32, the root one, Mods) have no `writable`
line, so every package is of the read-only class. The non-read-only databases are the `DirectoryFiles` folders
(Mods\Files, NonPackaged\Ini, UI\Layouts) and whatever the game registers from code. Code registrations found
(RegisterDatabase through the manager getter 0x004AFD20): the CAS part cache ("CAS/CASPartCacheService/OpenCachePackage"
0x005AE170, a writable DPF), 0x005BC6D0 (another DPF), the compositor caches (0x006CC570,
"CAS/CompositedTextureMediator", base packed-stream class; removed with priority -1000 at 0x006CC0A0), removals in
0x005E44C0 and 0x007D8E50 ("World/KeyList"). All of these are probed on every answer below them.

Threads that call FindProvider: the render thread (materials in the pending-node drain, finalize jobs 0x007297C0, CAS),
loader worker threads and the simulation thread.

### The cache

- **Key:** manager `this` and the 16 key bytes. **Value:** {package, priority, its index in the list, stored tick,
  write-epoch sum}.
- **Table:** 64k entries x 44 bytes = 2.8 MB, `VirtualAlloc` once, never freed (a thread may still be inside the hook
  after the feature turns off). Open addressing with linear probing (at most 64 slots). Each entry carries a stamp;
  entries whose stamp is not the table's current one are empty, so "empty the table" is `stamp++` (on a new generation,
  and when 75% full), never a memset. An SRW lock, shared for lookups and exclusive for stores, is never held while game
  code runs.
- **Generation:** `g_gen` is bumped before and after every RegisterDatabase (both slots), SetDatabasePriority and
  DatabaseChanged (hooked through their vtable slots; sites `RegisterDb`, `RegisterDbDerived`, `SetDbPriority`,
  `DbChanged`, cache layer only); `g_mutating > 0` while one runs. Meanwhile lookups go straight to the game and nothing
  is stored. A store happens only if the generation read before the game's lookup is unchanged after it and no change
  was in progress. The table belongs to one generation; a lookup under another generation sees it as empty.
- **Snapshot** per (manager, generation): list begin, size, an FNV fingerprint of all entries, and the index and pointer
  of every package that is not of the read-only class (the first 64). Built by the first store after a change, outside
  the lock.
- **Recheck** (`Hook_FindProvider` -> `Recheck`), for an entry of the current generation:
  1. younger than 60 s;
  2. the list is where the snapshot saw it (begin, size) and entry `index` is still {package, priority};
  3. the package still holds the key (one OpenRecord probe);
  4. none of the non-read-only packages above it holds the key (one probe each; answers with more than 32 such packages
     above them are never stored);
  5. the generation did not move meanwhile.

  All true: `*priorityOut = priority`, return the package. Otherwise the game's lookup runs and its answer is stored.
  Correctness: the game's scan returns the first holder; the stored index says every package above it did not hold the
  key when stored; read-only packages above it cannot have gained it (only a file change can, which the engine reports
  through DatabaseChanged = full invalidation); the others were just probed.
- **Reliability:** a store also requires every read-only package above the answer to have answered for sure (its key
  set present, or its file open after the lookup: `ReadOnlyAboveReliable`), so a transient open failure is not kept.
  This applies whether or not Remember missing files is on.
- **Changes the hooks could miss:** a store that finds the list moved (begin / size differ) under the same generation,
  or the full fingerprint (checked every 1,024 answers) differing, bumps the generation, logs once ("changed without
  RegisterDatabase / SetDatabasePriority") and counts it ("changes the hooks missed").
- **Start** checks the read-only class on the running build (its OpenRecord bytes, the base OpenRecord's "only
  dispositions 6 / 3" prologue reached through its slow-path CALL, DeleteRecord -> "return false",
  `CheckReadOnlyClass`). Then it hooks the four list methods first and FindProvider last.
- **Verification:** 1 answer in N and, on demand, every answer for 10 s also run the game's lookup and compare
  (package, priority) and the key bytes. Equal = counted; list changed during the check = inconclusive (not a
  difference); different = logged with both answers (key, package, vtable, priority, index), and the cache turns itself
  off for the session (the layer passes everything through; status "Turned itself off ..."). The game's answer is
  returned in every checked case.
- **Statistics:** lookups, answers from memory, game lookups (not found, re-check failed, passed through during
  changes), stored / not stored, entries, table restarts, list size, non-read-only packages, list changes, change
  notices, missed changes, checks; in developer mode also time in answers and in game lookups (QPC), the average of each,
  and "saved about X ms per second" (= answers x the average game lookup - the time in answers).

Cost per answer (inferred): one shared SRW acquire, a hash probe, a list read, 1 + (non-read-only packages above)
OpenRecord probes (about 0.1 us each) and a few atomics, against about 18 us for the game's 291 probes.

### Hooks

| Site | Slots | Layer |
|---|---|---|
| `FindProvider` | 0x00FB2DE0, 0x00FFE290 | `ResourceCache` (inside `FrameProfiler`) |
| `RegisterDb` / `RegisterDbDerived` | 0x00FB2DD4 / 0x00FFE284 | `ResourceCache` |
| `SetDbPriority` | 0x00FB2DDC, 0x00FFE28C | `ResourceCache` |
| `DbChanged` | 0x00FB2DEC, 0x00FFE29C | `ResourceCache` |

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| ResFindProvider / ResFindProviderSlot0/1 | 0x004AFFC0 / 0x00FB2DE0, 0x00FFE290 | Sig / SlotsOf(2) |
| ResRegisterDb / ResRegisterDbSlot | 0x004B2D00 / 0x00FB2DD4 | Sig / SlotsOf(1) |
| ResRegisterDbDerived / ResRegisterDbDerivedSlot | 0x00736A70 / 0x00FFE284 | Sig / SlotsOf(1) |
| ResSetDbPriority / Slot0/1 | 0x004B2EC0 / 0x00FB2DDC, 0x00FFE28C | Sig / SlotsOf(2) |
| ResDbChanged / Slot0/1 | 0x004B0960 / 0x00FB2DEC, 0x00FFE29C | Sig / SlotsOf(2) |
| ShadowedDbVtable | 0x00FFE078 | Sig (dword in the constructor 0x007342F0) |

Group `ResourceCache`.

### Source

`features/resource_cache.{h,cpp}`: `Start`, `Stop`, `Hook_FindProvider`, `Find`, `Recheck`, `Remember`,
`RoReliable` / `ReadOnlyAboveReliable`, `BuildSnapshot`, `MaybeFingerprint`, `Verify`, `Hook_RegisterDb*`,
`Hook_SetDbPriority`, `Hook_DbChanged`, `AcquireWatchers` / `ReleaseWatchers`, `CheckReadOnlyClass`, `TakeLookupNote`,
`GetStats`, `StatusText`, `ReportLine`, `RenderDeveloperUI`.

### Rules for maintainers

- Do not free the table or move the SRW lock: threads may still be inside the hook after Stop.
- Do not detour 0x004AFFC0's entry (the slots are its only references, and code bytes would be seen by the game-address
  self-check), and do not swap the FindProvider slots outside `SlotChain` (the profiler and the cache would lose each
  other).

## Rejected approaches

- Detouring FindProvider's entry: the vtable slots are its only references, and changed entry bytes would trip the
  game-address self-check. Details in [history](../../history/performance-resource-lookup-cache.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-resource-lookup-cache.md)
- [History](../../history/performance-resource-lookup-cache.md)
- [Main loop and services](../../engine/main-loop-and-services.md)

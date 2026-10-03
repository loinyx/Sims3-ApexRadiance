# Performance: Faster Game File Lookups, Lot Lighting While Moving, Faster Texture / Cache Compression, Spread New Objects, Faster Object Lookups

## Current development state

The optional Optimize rendering switch and its mode-dependent code paths are removed in the current development
branch. The renderer always uses the pre-2.5.6 code paths for those operations. Existing performance patches and
their settings are unaffected. Configurations that still contain `[ui].performance_mode` continue to load; the
unrecognized key is ignored and is omitted when the configuration is next saved.

The published 2.5.6 behavior and validation record are retained in Git history and in the release documentation.
In-game visual and performance validation of the removal is still required.

Optional registry timing covers draw and state callbacks, including nested work. Disable detailed timing for A/B measurements; see [frame-profiler.md](frame-profiler.md).

## Published 2.5.6 implementation record

The released Performance page had a live, persistent switch (`[ui] performance_mode`, default true). Explicit saved
false settings remained false; missing settings, the row reset and Reset all used true. It left visual settings and
existing performance patch choices unchanged. When enabled, it cached the prepared outdoor object lamp parameter
block in the exact-position lamp memo, indexed light-map entries in map-key order, read adjacent atlas/strength pixel
shader constants together, and reused immutable metadata for validated retained world-chunk textures. Disabling
restored the original map traversal, per-draw lamp-row preparation, separate constant reads and texture metadata
checks. No visible update was postponed.

The published offline constant-read tests passed 24577 checks, including bit preservation and failure fallback. Tests
compared 10000 lamp blocks byte for byte and 10000 map-index results with the original calculations, including
insertion, empty maps, reset and live mode switching. Those checks do not establish in-game image or performance
behavior; no FPS improvement was claimed.

## Historical implementation baseline (2026-09-29)

The following records the original implementation and test plan. Build-flavour, menu and unpublished-status notes are historical; use the current development section above and [UI documentation](../ui.md) for current behavior.

> Six anti-stutter features from the perf round 2 plan (`research\perf2\plan.md`, candidates C1, C7, C9, C4, C6 and C8), written
> on 2026-09-29 from the game's disassembly and Apex's own framework (no Sims3SettingsSetter code). Both builds (public
> and development). Menu: SYSTEM > **Performance** (one card), plus two Overview rows; developer lines under Developer >
> Profiler > "Performance".
>
> - **Faster Game File Lookups** (`[patches.ResourceLookupCache]`, experimental, **off by default**): remembers which
>   package answers each resource lookup (`ResourceMgr::FindProvider`) and re-checks the answer cheaply instead of asking
>   all ~290 packages again.
> - **Lot Lighting While Moving** (`[patches.LotLightingMotion]`, **on by default**): while the camera moves, the lot
>   lighting budget is scaled down (the current lot gets 3 ms instead of 15 ms per frame); the game's budget returns when
>   the camera stops.
> - **Faster Texture Compression** (`[patches.FastTextureCompression]`, experimental, **off by default**): the game's CPU
>   DXT1 / DXT5 encoders replaced by the same algorithm on four blocks at once; **bit-identical output** (C9, section
>   below). Sub-option "Use several cores" (default on): textures of 256 x 256 and more are split by rows of blocks over
>   a small pool of worker threads, same bytes.
> - **Faster Cache Compression** (`[patches.FastCacheCompression]`, experimental, **off by default**): the RefPack stream
>   write answered by a fast compressor in the game's stream format; the game decompresses it unchanged (C4, section
>   below).
> - Round 3 (`research\perf2\round3.md`, 2026-09-29):
>   - **Wall Shading While Moving** (`[patches.WallShadingWhileMoving]`, **on by default**): the wall ambient-occlusion
>     pass waits while the camera moves (at most 2 s per camera motion, then one pass per frame) and runs at most
>     once per frame.
>   - **Remember Missing Files** (`[patches.ResourceLookupMisses]`, experimental, **off by default**, needs the lookup
>     cache): "no package holds it" answers are kept too, and the writes of the DPF / DDF / packed-stream classes are
>     counted so unchanged packages need no re-check.
>   - **Faster File Lists** (`[patches.FileListCache]`, experimental, **off by default**): GetKeyList for the key-type
>     filter keeps each read-only package's keys per type.
> - **Spread New Objects Over Frames** (`[patches.SceneNodeBudget]`, experimental, **off by default**, added later on
>   2026-09-29): while the camera moves, Scene::BeginFrame's pending-node drain handles at most 512 nodes / 2 ms per
>   frame and the rest the next frames (C6, section below). Suspended in v1.8.0 over a node lifetime hazard; back (still
>   experimental, off by default) with a node lifetime guard: hooks on the node destructor, AddNode and the holder teardown.
> - **Faster Object Lookups** (`[patches.ObjectLookupIndex]`, experimental, **off by default**, added later on
>   2026-09-29): the object-by-ID lookup (a walk of the whole world object tree) answered from a validated index of
>   where the game's walk found each object (C8, section below).
>
> Status: implemented, **not yet compiled or tested in game** (the user compiles). Everything below marked VERIFIED was
> read in `research\engine_map\full.asm` / `S3SS-dev\re\TS3W.exe`; INFERRED = deduction, not confirmed at run time.

Related: [frame-profiler.md](frame-profiler.md) (the counters that measured both problems and that keep measuring them),
[../engine/lot-loading-and-streaming.md](../engine/lot-loading-and-streaming.md),
[../engine/room-light-maps.md](../engine/room-light-maps.md), [../engine/main-loop-and-services.md](../engine/main-loop-and-services.md),
[../engine/game-versions.md](../engine/game-versions.md) (signatures), [../ui.md](../ui.md) (the card).

## Purpose (measured baseline)

Dev profiler, the user's game, a ~110 s camera test, 1293 hitches:
- **Small hitches (8-16 ms):** "Resource lookup" dominant in ~65%: 246-288 lookups per hitch frame on the render
  thread, each asking ~291 packages (packages per lookup 291.1, misses 0), 4.6-5.3 ms per frame, i.e. ~18 us per lookup.
  Materials resolving textures (Scene::BeginFrame's pending-node drain), async-load finalize jobs, CAS and lot loading all
  go through it.
- **Medium hitches (16-50 ms) while the camera moves:** "Lot room solve" dominant in 50-77% at ~15-17 ms: the game's
  15 ms per-frame budget for the "priority" lot, spent in one frame.
- **Large hitches (50 ms+), later runs:** "DXT encode" is the dominant counter in 22-40% of them (44-49 ms per such
  hitch; also ~8-11% of the small ones), "RefPack compress" in 20-30% (20-37 ms per hitch; some small ones).

## User-facing settings

Menu: SYSTEM > Performance, card "Performance" ("Fewer stutters while you play"); no header switch, one row per feature
(the feature description, ending with the credit, on hover of its row). Overview rows "Faster File Lookups" and "Lot
Lighting While Moving" (switches; the names open the page). Search finds the rows ("Performance" breadcrumb).

| Row (label / description) | Feature / TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| "Faster game file lookups" / "Fewer small stutters when objects and textures load" | `[patches.ResourceLookupCache] enabled` | bool | **false** | - | Experimental. Off until the in-game checks below pass; then flip `enabledByDefault` in `patches/performance_patches.cpp`. |
| "Faster room lighting" / "Rooms light up sooner when you enter a lot or change floors" | `[patches.RoomLightQueue] enabled` | bool | **true** | - | Experimental (added 2026-09-29). Overview row "Faster Room Lighting". See "How it works: Faster Room Lighting". |
| "Spread lot lighting while moving" / "Lots relight in small steps while the camera moves" | `[patches.LotLightingMotion] enabled` | bool | **true** | - | |
| "Lot lighting time while moving" (shown while the switch is on) / "The current lot's time per frame while moving; 3 ms is the default" | `[patches.LotLightingMotion] budgetWhileMovingMs` | int | **3** | 1-15 | ms; end labels "Smoother" / "Lights sooner"; 15 = the game's own. Applied live (the hook reads it every call; `Update` clears the reinstall request). Never rename the key. |
| "Remember missing files" (shown while "Faster game file lookups" is on) / "Skips repeated searches for files no package has" | `[patches.ResourceLookupMisses] enabled` | bool | **false** | - | Experimental. Idle ("Waiting: needs Faster game file lookups") while the lookup cache is off. Includes the write epochs. |
| "Faster file lists" / "Fewer stutters when Sims load outfits and shapes" | `[patches.FileListCache] enabled` | bool | **false** | - | Experimental. Independent of the lookup cache. |
| "Wall shading waits while moving" / "Walls of new lots get their shading when you stop" | `[patches.WallShadingWhileMoving] enabled` | bool | **true** | - | Independent of "Spread lot lighting while moving" (it shares its camera detection). |
| "Faster texture compression" / "Fewer hitches when the game builds terrain, Sim and lot textures" | `[patches.FastTextureCompression] enabled` | bool | **false** | - | Experimental until the in-game checks below pass; then flip `enabledByDefault` in `patches/performance_patches.cpp`. No Overview row. |
| "Use several cores" (shown while the switch above is on) / "Large textures are shared out over several processor cores, with the same result" | `[patches.FastTextureCompression] useSeveralCores` | bool | **true** | - | Applied to the next texture (the hook reads it every call; `Update` clears the reinstall request). Off = every texture on the calling thread, as before. Never rename the key. |
| "Faster cache compression" / "Fewer hitches when the game stores Sims and objects in its caches" | `[patches.FastCacheCompression] enabled` | bool | **false** | - | Same. No Overview row. |
| "Faster Sim building" / "Fewer hitches when Sims are edited or change outfits" | `[patches.FastCasSort] enabled` | bool | **false** | - | Experimental (30/09), off by default until its in-game checks are confirmed. Bit-identical result, checked against the game's function on the first 16 calls. No Overview row. See "How it works: Faster Sim Building". |
| "Faster memory handling" / "Less waiting when the game hands out and frees memory" | `[patches.FastMemory] enabled` | bool | **false** | - | Experimental (30/09), off by default until tested in game. See "How it works: Faster Memory Handling". The record checksums have no row: they are part of Faster cache compression. |
| "Spread new objects over frames" / "Fewer hitches when a lot streams in while the camera moves" | `[patches.SceneNodeBudget] enabled` | bool | **false** | - | Experimental (C6). No Overview row; the tuning (nodes / ms per frame, longest wait) is developer-only and not saved. |
| "Faster object lookups" / "Fewer hitches when lot lights update; less script work" | `[patches.ObjectLookupIndex] enabled` | bool | **false** | - | Experimental (C8). No Overview row. |

Development build only (not saved): Developer > Profiler > "Performance" card: the cache's counters, "Check 1 answer in
N against the game" (default 64, 0 = never), "Check every answer for 10 s", the last difference; the lot lighting call,
camera source, last camera move, budget calls / scaled, last budget (game -> applied); texture compression: textures,
blocks (flat-luma, solid, encoded by the game's function), time, "checked textures: game X ms, Apex Y ms (Zx)", CPU
features, "Check 1 texture in N against the game" (default 8), "Check every texture for 30 s", the last difference;
several cores: state, worker threads created, split textures (chunks, % taken by workers, wall ms), "about X ms saved"
(summed chunk time minus wall time), textures on one core because another texture had the workers, FP state
mismatches (must be 0), "Worker threads per texture" (0..min(logical processors - 2, 6), default the maximum, 0 = one
core), "Split textures from (side, pixels)" (32..2048, default 256), "Default";
cache compression: streams, MB in / out, time, counting runs, writes after a counting run, did not fit, temporary
contexts, checks (game's decoder), the comparison with the game's compressor, "Check 1 stream in N by decompressing"
(default 1), "Also run the game's compressor on 1 stream in N" (default 0), "Search depth" (default 32); round 3: the
cache's lines "Missing files (on / off): answered from memory, remembered, not remembered", "Write epochs: counted
classes ...; answers with no probe of them, sums refreshed, writes counted", "File list cache: ..." and "File list
checks"; the wall shading gate's step / pass counters and "Longest wait of a pass while moving (ms)" (default 2000); scene node
budget: drain call, drains (game's while still / game's after a too-long wait / with a budget), nodes processed with a
budget, frames that left nodes, node-frames waiting, largest backlog, the last budgeted drain (done, left, ms), sliders
"Nodes per frame while moving" (default 512), "ms per frame while moving" (default 2.0), "Longest wait (ms)" (default
500); object lookups: lookups, from the index, game walks (not found), too old, path changed, passed through, stored /
not stored, entries, table restarts, container / object classes recognised, per-second rates and "saved about X ms",
average walk vs answer, "Check 1 answer in N against the game" (default 64), "Check every answer for 10 s", checks
(equal / different / inconclusive), the last difference.

All these features take part in undo (the menu's state capture covers every `[patches.*]` table) but not in Profiles (only
the look features are profile features).

## How it works: Faster Game File Lookups (C1)

### The game side (Steam 1.67.2; VERIFIED)

`ResourceMgr::FindProvider` **0x004AFFC0**, thiscall(key*, int* priorityOut), ret 8:
1. Locks the manager's `EA::Thread::Mutex` at `this+0x48` (0x004E16F0 with the infinite-timeout constant 0x00FB2CD0).
2. Reads `begin = [this+0x30]`, `end = [this+0x34]` **once** and walks the 8-byte entries `{Database*, int priority}`.
3. Per entry: unlock (0x004E17B0), `db->vfunc+0x34(key, 0, 1, 6, 1, 0)`, relock. The first `true` wins:
   `*priorityOut = entry.priority`, return `db`. None: return 0, `*priorityOut` untouched.
- Reached only through vtable slot +0x40 of the base resource manager vtable **0x00FB2DA0** (slot 0x00FB2DE0) and of the
  derived ResourceSystem one **0x00FFE250** (0x00FFE290), and through the wrapper 0x004AFDA0 (slot +0x44, calls +0x40).
  No direct CALL.
- **The "cookie" out-parameter is the winning package's priority** (the list's second dword). So the result
  (package, priority) is fully decided by which list entry answers first.
- `db->vfunc+0x34` is the database's OpenRecord(key, record**, access, disposition, flag, info*): with no record and no
  info asked (arguments 2 and 6 = 0) it only answers "do you hold this key?".

The package list (the only code that writes `[mgr+0x30..0x38]`; its vector helpers 0x004B1F50, 0x004B25B0, 0x004B0B90
have no other callers):

| Address | Slot | What | Convention |
|---|---|---|---|
| 0x004B2D00 | +0x34 of 0x00FB2DA0 (0x00FB2DD4) | RegisterDatabase(bool add, db*, int priority): add = insert before the first entry of lower priority (list sorted highest first, ties in registration order), after IsRegistered (+0x38, 0x004AFF70) refuses duplicates; then `db->vfunc+0x48(1, mgr, 1)` (Attach). Remove = Attach(0, ...) then erase. AddRef / Release through `db+4`. | thiscall, ret 0xC, returns bool |
| 0x00736A70 | +0x34 of 0x00FFE250 (0x00FFE284) | ResourceSystem's override (its own name map at +0x2A0 etc.), calls 0x004B2D00 **directly** (0x00736C69) | same |
| 0x004B2EC0 | +0x3C of both (0x00FB2DDC, 0x00FFE28C) | SetDatabasePriority(db*, int): erase + sorted re-insert | thiscall, ret 8 |
| 0x004B35A0 / 0x007366A0 | +0 | destructors (0x004B30B0 frees the vector): game exit | |
| 0x004B0960 | +0x4C of both (0x00FB2DEC, 0x00FFE29C) | **DatabaseChanged(db*, keyVector*)**: the engine's own "these keys of db changed" notice; runs FindProvider (+0x44) per key and calls the resource cache's listeners (`mgr+0x10` list) for keys whose answer is now `db` or nothing | thiscall, ret 8 |

Other slots: +0x38 IsRegistered, +0x48 0x004B17E0 = list every package holding a key (not a writer), +0x54 0x004B3200 =
a different list at +0xA0 (factories), not the package list.

Who sends DatabaseChanged: the ResourceSystem's file watcher (`ResourceSystem/ShadowWatcher`): its update 0x00737560
pops changed paths and calls ResourceSystem vfunc+0x14 = **0x00734D10**, which for each package object with that path
calls outer+0xC (0x007343C0 "file changed?": size / time vs +0x100 / +0xF8, drops the key set) and, if changed,
GetKeyList then `mgr->+0x4C(db, keys)` (0x00734DBF). The loose-file folder databases do the same after a rescan
(0x004A4160).

**Package classes** (every constructor that stores the IDatabase base vtable 0x00FB21F8; all have OpenRecord with
`ret 18h`):

| Vtable | Constructor | What | Can gain keys at run time? |
|---|---|---|---|
| **0x00FFE078** | 0x007342F0 ("ResourceSystem/ShadowedDBPF"; inner object at outer+8, outer vtable 0x00FFE0D0) | Every `PackedFile` line of a Resource.cfg without `writable` (0x00737D70 -> 0x00737950): the game's packages, the EP / SP packages, Mods\Overrides / Packages, DCCache .dbc files. Closes idle files; keeps a key set (`this+0xB0`) meanwhile. | **No**, except when its file changes on disk (VERIFIED below) |
| 0x00FFD790 | 0x0072CC60 | base packed-stream DBPF (read-only OpenRecord, but Open / Close change what it holds) | treated as yes |
| 0x00FB2600 | 0x004A8F70 ("ResourceMan/DPF") | `PackedFile <path> writable` (opened with access 3) and many world / save / cache callers | yes |
| 0x00FB2420 | 0x004A5500 ("ResourceMan/DDF") | `DirectoryFiles <folder> [autoupdate]` (loose files) | yes |
| 0x00FFD5F8 | 0x0072B8F0 ("ResourceSystem/MemoryDB") | in-memory databases | yes |
| 0x01046EE8 | 0x0098B150 (social cache) | socialCache.package | yes |

The read-only class in detail (VERIFIED): OpenRecord **0x007345D0**: with no record / info asked and a key set present it
only probes the key set (0x0072DBD0 -> 0x0072DAD0: hash `key[0] ^ key[3]`, `div` by the bucket count, chain walk
0x005492E0 comparing 16 bytes) under the package mutex (`this+0x40`, 0x0072C6C0); otherwise AcquireOpen (slot +0x50,
0x00734710: opens the file with access 1 = read, drops the key set), the base OpenRecord **0x0072D470** (accepts only the
open-existing dispositions 6 and 3: it can never create a record), Release (slot +0x54, 0x007347C0). DeleteRecord (slot
+0x40, 0x007346A0) calls 0x00624F70 = `xor al,al; ret 4`. So its key set is the file's index; it changes only when the file
on disk changes, which the watcher reports through DatabaseChanged. The user's Resource.cfg files (Game\Bin,
GameData\Shared, GameData\Win32, the root one, Mods) have no `writable` line: every package is of this class; the
non-read-only ones are the `DirectoryFiles` folders (Mods\Files, NonPackaged\Ini, UI\Layouts) and whatever the game
registers from code. Code registrations found (RegisterDatabase through the manager getter 0x004AFD20): the CAS part
cache ("CAS/CASPartCacheService/OpenCachePackage" 0x005AE170, a writable DPF), 0x005BC6D0 (another DPF), the compositor
caches (0x006CC570, "CAS/CompositedTextureMediator", base packed-stream class; removed with priority -1000 at
0x006CC0A0), removals in 0x005E44C0 and 0x007D8E50 ("World/KeyList"). All of these are probed on every answer below them.

Threads (plan section 2.2, MEASURED): render thread (materials in the pending-node drain, finalize jobs 0x007297C0,
CAS), loader worker threads, simulation thread.

### The cache (features/resource_cache.cpp)

- **Key** (manager `this`, the 16 key bytes). **Value** {package, priority, its index in the list, stored tick, write-epoch
  sum}. Found keys; since round 3 also "no package holds it" (package 0) when Remember Missing Files is on (below).
  Since round 3 a store also requires every read-only package above the answer to have answered for sure (its key set
  present or its file open after the lookup, `ReadOnlyAboveReliable`), so a transient open failure is not kept. An
  absent answer (package 0) also needs every read-only package of the list to have been reliable **before** the game's
  lookup (`roBefore`, computed in `Hook_FindProvider` only while Remember Missing Files is on).
- **Table:** 64k entries x 44 bytes = 2.8 MB, `VirtualAlloc` once, never freed (a thread may still be inside the hook
  after the feature turns off). Open addressing, linear probing (at most 64 slots). Each entry carries a stamp; entries
  whose stamp is not the table's current one are empty, so "empty the table" is `stamp++` (on a new generation, and when
  75% full), never a memset. SRW lock: shared for lookups, exclusive for stores; **never held while game code runs**.
- **Generation:** `g_gen` is bumped before and after every RegisterDatabase (both slots), SetDatabasePriority and
  DatabaseChanged (hooked through their vtable slots); `g_mutating > 0` while one runs. Meanwhile lookups go straight to
  the game and nothing is stored; a store happens only if the generation read before the game's lookup is unchanged after
  it and no change was in progress (so an answer computed across a list change is never kept). The table belongs to one
  generation; a lookup under another generation sees it as empty.
- **Snapshot** per (manager, generation): list begin, size, an FNV fingerprint of all entries, and the index + pointer of
  every package that is **not** of the read-only class (the first 64). Built by the first store after a change, outside
  the lock.
- **A lookup** (`Hook_FindProvider`): entry found for the current generation -> `Recheck`:
  1. younger than 60 s;
  2. the list is where the snapshot saw it (begin, size) and entry `index` is still {package, priority};
  3. the package still holds the key (one OpenRecord probe, exactly as the game asks);
  4. none of the non-read-only packages **above** it holds the key (one probe each; answers with more than 32 such
     packages above them are never stored);
  5. the generation did not move meanwhile.
  All true: `*priorityOut = priority`, return the package. Otherwise the game's own lookup runs and its answer is stored.
  Correctness argument: the game's scan returns the first holder; the stored index says every package above it did not
  hold the key when stored; read-only packages above it cannot have gained it (only a file change can, which the engine
  reports through DatabaseChanged = full invalidation); the others were just probed.
- **Changes the hooks could miss:** a store that finds the list moved (begin / size differ) under the same generation,
  or the full fingerprint (checked every 1024 answers) differing, bumps the generation, logs once
  ("changed without RegisterDatabase / SetDatabasePriority") and counts it ("changes the hooks missed").
- **Start** checks the read-only class on the running build (its OpenRecord bytes, the base OpenRecord's "only
  dispositions 6 / 3" prologue reached through its slow-path CALL, DeleteRecord -> "return false"); if that fails nothing
  could be cached, so the feature refuses to start. Then it hooks the four list methods first and FindProvider last.
- **Verification (development build):** 1 answer in N (default 64) and, on demand, every answer for 10 s: the game's own
  lookup runs too and its (package, priority) and the key bytes are compared. Equal = counted; list changed during the
  check = inconclusive (not a difference); different = logged with both answers (key, package, vtable, priority, index)
  and **the cache turns itself off for the session** (the layer passes everything through; status "Turned itself off
  ..."). The game's answer is returned in every checked case.
- **Statistics:** lookups, answers from memory, game lookups (not found, re-check failed, passed through during changes),
  stored / not stored, entries, table restarts, list size, non-read-only packages, list changes, change notices, missed
  changes, checks; development build: time in answers and in game lookups (QPC), average of each, "saved about X ms per
  second" (= answers x the average game lookup - the time in answers).

Cost per answer (INFERRED): one shared SRW acquire, a hash probe, a list read, 1 + (non-read-only packages above) OpenRecord
probes (~0.1 us each), a few atomics; against ~18 us for the game's 291 probes.

### Hooking and the Frame Profiler (framework/slot_chain.{h,cpp})

FindProvider is reached only through vtable slots, and both the profiler (dev build, counter "Resource lookup") and the
cache wrap it. `SlotChain` gives each wrapper a fixed layer (0 = gate, used only by the wall shading gate; 1 = Frame Profiler; 2 =
Resource cache; 3 = fast compressor; lower = outer),
whatever the install order:
- the slots hold the outermost installed layer's hook; each hook calls `SlotChain::Next(site, layer)` = the next inner
  installed layer's hook, or the game function;
- install: the new layer's next pointer first, then either the slots are swapped to it (one interlocked
  compare-exchange per slot while the page is writable; every slot must hold the expected pointer or nothing is written)
  or the outer layer's next pointer is re-pointed to it (one atomic store);
- remove: the reverse; a removed hook keeps its next pointer (threads inside it finish normally).
The profiler's `AttachSlots` / `DetachSlots` use it for every shared slot (`kSharedSlots`: FindProvider, the RefPack
write, the wall AO step, both GetKeyList slots). So the
profiler always times every call (answers from memory included) and the cache sees every call. The profiler reads
`ResourceCache::TakeLookupNote()` after each call: an answer from memory adds "from cache" and counts the packages the
cache asked as "packages probed". The list methods (sites RegisterDb, RegisterDbDerived, SetDbPriority, DbChanged) use
the same mechanism with only the cache layer.

## How it works: Faster Room Lighting (room lighting queue, 2026-09-29)

Study of 29/09 (5 parallel studies, report in the session scratchpad `plan.md`): from entering a lot to the last room
solve took 12-36 s, with only 1.2-2.8 s of solve work in it; in 95-98 % of the frames with solve work a single lot story
was being solved. The cause is the game's queue, not the solve itself.

### The game side (Steam 1.67.2; VERIFIED in full.asm)

- One room at a time for the whole world. The scheduler `0x006C5C20` (fastcall(tree = lightMgr+0xD4), plain `ret`),
  reached by the tail `jmp` at `0x006C5E39` of the per-frame light tree update `0x006C5E20` (after the rooms' gathers),
  returns at once while `[tree+0x74]` (the current room) is set; else it asks `0x006A8190` for (room, priority) of every
  room of every level of every lot, sorts them, and makes the best one current (above 0.001; `0x0069E860`: state 3, the
  manager's polling set). The current room is cleared only by FinalizePrime (`0x006A0E00 -> 0x006C4870`) or an
  invalidate. The lot pass (`0x00ADB8F0 -> 0x006A8BA0 -> 0x006A88B0 -> 0x006A3F80`) solves it within its lot's budget
  (5 / 10 / 15 / 30 ms), and the next room is picked only on the next frame.
- The priority (`0x0069E770`, fastcall(room), float in ST0; its only call is `0x006A81DF`): 0 unless state 2; class 0:
  10000 x {1 camera story or outdoor below, 0.8 indoor below, 0.5 above} x (0.5 on a lot not in high quality); class 1:
  1000; class 2: 100; 0 when the class is above LodChoice. So every pending first solve of every loaded lot goes before
  any upgrade of the viewed story.
- The LOD ladder (`0x0069EA70`, at the end of every solve): class 0 -> 1 -> 2, one full solve per step (wall rows 4 / 7 /
  13). An invalidate (`0x0069EED0`, `0x0069F160`) restarts at class 0 unless the room was solved before (`+0x100 != 4`)
  AND its shown class is at least LodChoice (the `jl` at `0x0069EF58` / `0x0069F1C5`).
- An invalidate of the room being solved throws its work away (`0x6C4870`, then `0x69E950(0)`: no commit).

### The patch (`features/room_light_queue.cpp`, feature `RoomLightQueue`)

Each part is checked against the Steam bytes and stays off when they differ; every write goes through
`MemPatch::WriteCodeSuspended`, and Stop puts the bytes back.

1. Viewed lot first: the CALL at `0x006A81DF` -> `PriorityHook`: the game's value x4000 for rooms of the priority lot on
   the camera's story, x2000 below it (seen from above); other rooms and 0 unchanged. The priority lot is the one the
   game gives 15 ms (`0x006FDE10` SceneObjectManager, `0x006FDC80` against its +0x10D0 / +0x10E0), with the story
   manager's lot id `mgr+0x90` / `+0x94`.
2. No middle step: `0x0069EAA2` `BF 01 00 00 00 8D 5F 01` -> `8B F8 BB 02 00 00 00 90` (mov edi,eax; mov ebx,2): a
   finished class-0 solve steps straight to LodChoice.
3. Requeues keep the class: the two `jl` (`7C 0B`, `7C 02`) -> `90 90`: a room solved before goes straight to its target
   class.
4. Several rooms per frame: the `jmp` at `0x006C5E39` -> `PickHook`: the scheduler, then, only on the render thread (id
   taken at Present) and only when the pick made a new room current that belongs to the priority lot (story built,
   `mgr+0x280`), the room is solved at once (`0x006A3F80` with a game stopwatch of its own: `0x004F35B0` kind 4 = ms,
   `0x00408700`, `0x004F33C0`) and the next one picked, until 4 ms (1 ms while the camera moves) or a room that did not
   finish (the lot pass goes on with it next frame).
5. A room waiting in state 2 at a class above its LodChoice (its story left the camera's after it was sent at a higher
   class) gets priority 1 instead of 0 (the game would leave it until the camera comes back): solved last.

Review of 29/09 (adversarial agent): the drain runs only when the room current at the previous pick is done and the new
one is of that same lot (so the lot pass of that lot ran: not paused by +0x18 / +0x4E) and is the priority lot's; Apex's
QueueRoom holds back only the requeues after a setting or ambient change (`defer`), never a requeue caused by a lamp list
change (its list may point to a lamp being deleted), and only while the room update can send it later; a whole-world
relight asked while the story share is off runs at once.

Apex's own requeues (`features/level_light_share.cpp`, same day): `QueueRoom` never invalidates the room being solved
(state 3): it is sent again when that solve is over (`FlushDeferred`, from the room update); whole-world relights asked in
a burst run once, 250 ms after the last ask; the settle requeue is armed once per lot state; the per-point hooks return
at once for a light the game is about to drop (colour sum under room+0x63C, the game's own sums in the same order).
Night Lighting no longer relights every lot 3 s after a world loaded at night (the game has just solved them).

Status (Developer > Profiler): "viewed lot first on (N of M priorities raised), no middle step, requeues keep the class,
several rooms per frame (frames, extra solves, finished, ms)" and the game's own solve time per class
(`0x011D1200` / `04` / `08`, ms, added by `0x006C2380`).

## How it works: Lot Lighting While Moving (C7)

### The game side (VERIFIED)

- **0x00ADB8F0** lot lighting update, thiscall(lot lighting manager), from the lot renderer update 0x00AEB2E0 ->
  0x00AE4CB0 (render thread, per lot with lighting work, every frame; also reached from the lot impostor builder
  0x00AD9E30 -> 0x00AEB3F0). Returns at once unless `[this+0x18]` and not `[this+0x4E]`. Starts an EA stopwatch in ms
  (0x004F35B0(4, 0), 0x00408700), then **`call 0x00ADB120` at 0x00ADB95D**; `fst [esp+0Ch]` keeps the budget for the loop
  test while ST0 stays loaded. For each level object of the lot (deque at `this+0x24..0x40`): `0x006A8BA0(stopwatch,
  budget)` (the CALL at 0x00ADB9AD), then `elapsed = 0x004F33C0`; stop when elapsed > budget. At least one level runs.
- **0x006A8BA0** per level: its two light solvers (vfunc +0xC of `this+0x290` / `+0x2E8`) and the dirty rooms
  (0x006A88B0 -> ... -> 0x006A3C90), all with (stopwatch, budget). **0x006A3C90** is a resumable state machine (state at
  room+0xEC, 9 = done): while room+0x164 is set it stops as soon as elapsed >= budget and continues next frame; rooms
  without +0x164 finish in one go. A smaller budget spreads the work, it skips nothing.
- **0x00ADB120** budget, thiscall, ret, result in ST0 (its only caller is 0x00ADB95D). Values read in TS3W.exe:

| Case | Test | Budget |
|---|---|---|
| Tool mode | WorldManager `[0x011ECBC4]` +0x1B4 == 0 | `[0x011833DC]` = 1000 ms |
| Priority lot, loading | 0x006FDC80(SceneObjectManager, lot id) and manager +0x4F | `[0x011833D8]` = 30 ms |
| Priority lot | 0x006FDC80 true | `[0x00F9A62C]` = 15 ms |
| Other lot, loading | manager +0x4F | `[0x01045CCC]` = 10 ms |
| Other lot | | `[0x00FBD498]` = 5 ms |

- Lot id = `[[manager+0x14]+0x48/+0x4C]`. **Priority lots** = the two lot ids at SceneObjectManager (`[0x011D1CF8]`)
  +0x10D0 / +0x10E0, copied from +0x10D8 / +0x10E8 in 0x00701270 (writers 0x00703510, 0x00703D60, 0x007042F0, all
  renderer / object-hiding code): most likely the active or focused lot (INFERRED, not proven). The measured 15-17 ms
  "Lot room solve" hitches are this 15 ms budget.
- Why the current lot keeps relighting while the camera moves is still open (plan section 10, question 2); Night Lights
  also queues room relights (below).

### The patch (features/lot_lighting_motion.cpp)

- The 5 bytes of the CALL at 0x00ADB95D become `call Hook_LotLightBudget` (`MemPatch::WriteCodeSuspended`: every other
  thread suspended and none stopped inside the 5 bytes; restored the same way, only if it still points to Apex's hook).
  Start checks that the CALL reaches 0x00ADB120 and that the next instruction reads ST0 (`D9` / `DD`).
- `float __fastcall Hook_LotLightBudget(mgr, edx)`: calls 0x00ADB120 (a float return is ST0 in every x86 convention; the
  caller's x87 stack is empty at the call), samples the camera, and while the camera moved in the last 300 ms returns
  `max(0.25, min(game, game x budgetMs / 15))` for budgets under 100 ms; else the game's value. At the default 3 ms: the
  priority lot 15 -> 3 ms (30 -> 6 while loading), other lots 5 -> 1 ms (10 -> 2 while loading): the engine's own order
  and the priority lot's larger share stay; only the time per frame shrinks. The tool mode's 1000 ms is never changed.
- **Camera motion:** the camera eye, `[[root]+0x24]+0x60`, the read WorldManager::Update does at 0x00C6D5BD..0x00C6D5C9
  (`call 0x006E8330` = `mov eax,[0x011D1860]; ret`, `call 0x006E8400` = `mov eax,[ecx+24h]; ret`, `movaps xmm0,[eax+60h]`;
  its y is compared with the terrain height threshold WorldManager+0xE8). The root global, the camera offset and the
  eye offset are parsed from those bytes at Start on every build. Moving = any eye component changed by more than 5 mm
  since the previous sample, or by more than 5 mm within 100 ms (slow pans and zooms); it lasts 300 ms after the last
  change. Orbit, zoom and pan all move the eye; following a walking Sim does too. SEH-guarded read; not readable (no
  world) = not moving. Sampled inside the hook and, since round 3, once per frame from a Present callback
  (`D3D9Hooks::RegisterPresent("LotLightingMotion")`, registered while this feature or Wall Shading While Moving is on):
  a stale eye after a quiet spell no longer reads as a false "moving" (round3.md section 2.3).
- The Frame Profiler keeps both its lot lighting targets (the entry of 0x00ADB8F0, Detours; the CALL 0x00ADB9AD): other
  bytes. Its "Lot room solve" calls now receive the scaled budget.

## How it works: Wall Shading While Moving (round 3, section 2)

Measured (round3.md section 2): with Lot Lighting While Moving on, every moving "Lot room solve" hitch of 5 ms or more is
one wall ambient-occlusion (AO) pass: 10-17 ms each, 54-108 ms when lots load (the AO ray loop `0x0068AF31` in 85% of
the samples of the 108 ms case). The pass never looks at the budget.

### The game side (VERIFIED in full.asm)

- Every lot level embeds two solvers, `lvl+0x290` (wall AO, vtable 0x00FF0594) and `lvl+0x2E8` (vtable 0x00FF0714).
  0x006A8BA0 (the room solve, from the lot lighting update) calls slot +0xC of both when `[lvl+0x88] >= 0`.
- **0x00688920, the solver driver** (slot +0xC of both vtables), thiscall(stopwatch*, float budget), ret 8:
  `if ([s+0x14] != 2 && [s+8]->vfunc+0xC()) [s+0x14] = s->vfunc+0x1C(stopwatch, budget)`. The step's return value
  becomes the state; the driver asks again every frame until it is 2. The CALL is `FF D2` at +0x2B; its return address
  (+0x2D) is `89 46 14` (the store).
- **0x0068B810, the wall AO step** (slot +0x1C of 0x00FF0594 = **0x00FF05B0**, its only reference), thiscall, ret 8,
  returns the next state:
  - level `[s+4]`, outdoor room = RoomById(level, `0x005BFB90()`) (0x006A6550). No room: returns
    `[level+0x280] ? 2 : 0`. No walls (`(room+0xDC - room+0xD8) / 4 == 0`): `[s+0x18] = 0`, returns 2.
  - **state 1 (refinement):** `v = [s+8]->vfunc+0x10()`; **v < 0: returns 1**, i.e. the engine's own "not now, try again
    next frame"; else picks the highest detail level `i < MaximumDetailLevel` whose predicted cost stays under AO.ini
    WallMillisecondsBudget (`[0x011CF4A0+0x24]`, int ms, 10); none: 2; else one pass at that level, returns 2.
  - **other states (0, first pass):** one pass at detail 0, `[s+0x18]` = its elapsed ms, returns 1.
  - a pass: lock the AO image (0x00618DF0), `0x0068B2B0` for every wall, unlock (0x00619160). The stopwatch is read
    only after the loop; **the budget argument is not used at all**.
- **Who waits for it:**
  - 0x00688DB0 binds the AO image to the walls only when the state is 1 or 2 (before the first pass: walls without AO).
  - **Lot load stage 20** (case 20 of the jump table 0x00AEB280 in the lot load state machine 0x00AEA680) calls
    0x00ADBBA0 -> 0x006A5B50 for every level: both solvers' state != 0. Not yet: the stage yields and retries next
    frame. Then it clears the lot lighting "loading" flag `[+0x4F]` (budgets 10/30 -> 5/15) and stage 21 sets the lot
    renderer's "loaded" flag `[+0x1E]`, which the lot's render managers (0x00AE4D80) and the impostor LOD switch
    0x00AD9E30 (returns "retry" (7) while not loaded) wait for. **So a lot finishes loading only after the first pass
    of all its levels.**
  - 0x006A5BF0 (both states == 2) <- 0x00ADBC30 <- 0x00AE06B0 (a jump thunk from the lot renderer, +0x240) <- the
    ThumbnailManager's lot capture (CALL 0x00D5BE2F, "UI/ThumbnailManager", state machine near 0x00D5A2E0): a lot
    thumbnail returns "not yet" until every level's refinement ran. Nothing else waits for the refinement.
- Resets to state 0: slot +0x10 0x006895C0 (from message 0x0486519D through slot +4, and 0x006A4240 / 0x006A4180) and
  level creation.
- **Other callers of the driver:** the synchronous level solve **0x006A4180** (from the lot LOD switch setup 0x00ADBAD0 <-
  0x00AEB3F0 <- 0x00AD9E30): resets both solvers and drives each once with the 60,000 ms budget `[0x00FF3460]`. The tool
  mode passes 1000 ms. The impostor pump 0x00AD97E0 runs the lot pass (0x00C7CEA0) only in the tool mode.

### The gate (features/lot_lighting_motion.cpp, `StartWallAo` / `Hook_WallAoStep`)

- Slot swap of 0x00FF05B0 through `SlotChain` (site `WallAoStep`, **layer `Gate`, the outermost**: the gate recognises
  the driver by its own return address, so nothing may sit outside it). No code bytes change.
- Start checks the whole driver body (52 bytes) and the step's first 69 bytes (`kStepBytes`) against the studied code; any difference:
  the feature stays off and says so.
- The gate acts only when its return address is the driver's (+0x2D) and `0 < budget < 100 ms` and the state is 0 or 1.
  Everything else (the synchronous solve, the tool mode, any unknown caller or state) goes to the game unchanged.
- **While the camera moves** (`LotLightingMotion` camera, 300 ms hold): the first pass (state 0) returns 0 and the
  refinement (state 1) returns 1 without a pass. **One wait per state and per camera motion:** it starts at the first
  deferral of that state since the camera was last seen still and lasts at most `firstPassWaitMs` (default 2000 ms,
  Developer slider "Longest wait of a pass while moving"). Once it ran out, the pending passes of that state run for the
  rest of the motion, one per frame (the per-frame limit below); the wait is not restarted by those passes (restarting it
  made every other lot start a fresh wait). **Everything resets when the camera is seen still** (`OnPresentSample`
  clears both waits at every Present without motion; a pass that runs while still clears its own), so the next motion
  defers again. When the camera stops, the waiting passes run one per frame.
- **Always:** at most one pass of >= 1 ms per frame across all lots (a frame = between two Presents, counted by the
  Present callback; when no Present came for 250 ms, a frame is 33 ms). Passes that take under 1 ms (no room, no walls)
  do not use up the frame.
- Returning the current state is exactly what the engine does itself in state 1 when its estimate is negative: the
  driver stores it unchanged and asks again next frame. Nothing else reads the step's return value.
- **Visible effect:** outdoor walls of a lot that loads while you pan get their AO shading when the camera stops (or
  after 2 s), one level per frame; such a lot finishes loading (shown instead of its impostor) up to about 2 s later
  while panning. The AO detail itself is unchanged (the refinement still picks its level from WallMillisecondsBudget).
- **Not done:** capping WallMillisecondsBudget while moving (round3 L2): it only shortens the refinement and would change
  the detail chosen for good; the deferral covers both passes. Making the pass resumable per wall (L3): a
  re-implementation, not needed unless the deferral looks bad.
- Counters (Developer > Profiler > Performance): step calls, first passes / refinements run, held while moving (first /
  refinement), passes run after the wait, moved to a later frame, passed through, pass time (average, longest,
  last). The Frame Profiler's "Wall AO pass" counter (inner layer) times only the passes that run.

## How it works: Remember Missing Files (round 3, sections 3.3 and 3.4)

Measured (round3.md section 3): with the lookup cache on, ~36% of FindProvider calls are keys that exist in no package
(the resolve 0x007D8110 looks a key up and, on a miss, looks up the variant 0x007D7580 builds: group ^ 0x08000000, and
for 5 types with instance-high 0 also instance ^ 0x08000000). Each costs a full scan (~19 us) and together they are 78%
of the remaining lookup time; cache hits still probe ~17 non-read-only packages (~1.3 us each hit).

### Absent entries

- The game's 0 answer is stored like a found one (provider 0, index "all": every non-read-only package counts as above
  it), under the same generation rules, and re-checked by probing every non-read-only package; any "yes": the game's
  lookup runs and replaces the entry. On an answer from memory the cache returns 0 and leaves `*priorityOut` alone, as
  the game does.
- **Why it is correct:** read-only packages cannot gain keys without RegisterDatabase / DatabaseChanged (generation);
  every other package is probed (or its writes are counted, below).
- **The one read-only exception (VERIFIED, 0x007345D0 / 0x00734710):** a read-only package with no key set
  (`[db+0xB0] == 0`) whose file cannot be opened at that moment answers "no" for every key. After the game's lookup,
  such a package is closed with no key set (`[db+0xB0] == 0 && [db+0x14] == 0`; a successful probe leaves it open, and
  the idle close builds the key set before closing). `Remember` checks every read-only package above the answer (all of
  them for "absent") and stores nothing when one is in that state (counter "not remembered (a read-only package could
  not answer for sure)"). This check now also protects found answers (it applies whether or not Remember Missing Files
  is on).

### Write epochs (zero-probe answers)

The probe is OpenRecord(key, 0, 1, 6, 1, 0). For each non-read-only class, every code path that can change its answer
was traced (three read-only studies of full.asm, 2026-09-29; V = read in the disassembly):

| Class (vtable) | What the probe reads | Writers hooked (slot: function) | Bump when |
|---|---|---|---|
| DPF, writable package (0x00FB2600; derived 0x01048DA0 overrides only +0x7C / +0x84) | hash index `[this+0x2D0]` (vfunc +0x28) under the mutex +0x270; closed: auto-open 0x004A6860 -> slot +0x18 | +0x00 dtor 0x004A8D00 / 0x00996630, +0x08 shutdown 0x004A6AE0, +0x18 open 0x004A76E0, +0x1C close 0x004A8D20, +0x24 flush / compaction 0x004A9B70, +0x34 OpenRecord 0x004A94C0, +0x3C CloseRecord 0x004A8E00, +0x40 DeleteRecord 0x004A85A0, +0x5C set index 0x004A6690, +0x8C load index 0x004A9950, +0x9C convert index 0x004A6C80; **entry** of the non-virtual direct write 0x004A7FC0 (callers 0x007D7A50 / 0x007D7B30 / 0x007D7C10, KeyList copies; 0x004AE510, compaction temp) | always, except OpenRecord (a record asked with write access or a disposition other than 6 / 3: insert or replace) and CloseRecord (`[rec+8] == 0x12E4A892`, a writable record: commit 0x004A8910 removes and re-inserts the key, and leaves it removed on failure) |
| DDF, loose-file folder (0x00FB2420) | map `[this+0x50]` under the mutex +0x80 when `[this+0x0C]` | +0x00 dtor 0x004A6030, +0x08 0x004A3950, +0x18 open 0x004A3AD0 (full rescan), +0x1C close 0x004A5240, +0x2C set location 0x004A4370, +0x34 OpenRecord 0x004A62B0, +0x40 DeleteRecord 0x004A52A0, +0x58 one-file refresh 0x004A6100, +0x5C rescan 0x004A5340 (both also from the directory watcher thread, through the vtable; DatabaseChanged comes **after** the change) | always, except OpenRecord: whenever a record is asked (it inserts a key whose file appeared on disk even for reading) |
| Packed stream (0x00FFD790; not the read-only class) | open mode `[this+0x14]` and index `[this+0x70]` | +0x00 dtor 0x0072CD30, +0x08 0x0072C6A0, +0x18 open 0x0072D790, +0x1C close 0x0072C8F0, +0x2C set location 0x0072CD50 (it cannot create or delete records) | always |
| MemoryDB (0x00FFD5F8) | not counted: the non-virtual PutResource 0x0072C350 (callers 0x007D7CF0 / 0x007D7DD0 / 0x007D7EB0, 0x00D57B90) inserts keys | - | always probed |
| ContentManager (0x01046EE8, downloaded content, -1200) | not counted: it forwards to sub-databases and its maps change in non-virtual install code (0x0098B5E0, 0x009905E0, 0x00989B50, 0x00984B60, 0x00987C20, ...) | - | always probed |

- Every studied function's first bytes are checked before its class is counted; a class with any difference, an
  unreadable slot, or a slot another module changed stays probed (all or nothing per class). The DPF classes also need
  the entry hook of 0x004A7FC0 (`EntryChain` site `DpfWriteDirect`, prologue `83 EC 28 53 56`, no jump lands inside
  it). Record objects need no hook: they close through the database's slot +0x3C (0x004AD9F0, dtor 0x004AE080).
- Counters (`WriteBegin` / `WriteEnd` around each hooked call): `g_writeBusy` (writes in progress), `g_writeSeq` (bumped
  at begin and end) and a per-database epoch in 1024 buckets by pointer (a shared bucket only costs an extra re-check).
  Nested calls (OpenRecord -> DeleteRecord, flush -> close / open, the probe's own auto-open) just count twice.
  `WriteEnd` runs in a `__finally` (`Bracketed`, since v1.5.0), so an exception unwinding through the game's write
  never leaves `g_writeBusy` raised (which would keep every epoch sum "unknown" until restart).
- An entry stores the sum of the epochs of its **counted** databases (the non-read-only ones above it, and its own
  package when it is not read-only). The sum is taken only when no write was in progress and none happened since
  before the game's lookup (seqlock on `g_writeSeq`), else "unknown".
- **Answer:** sum unchanged (read with no write in progress and the sequence stable around it) -> no probe of the
  counted databases, and none of a read-only answering package (same premise as the cache itself); databases of
  uncounted classes are still probed. Sum changed or unknown: every package is probed as before; if that passes with no
  write meanwhile, the entry's sum is refreshed (counter "sums refreshed"). A write therefore costs each affected entry
  one probe round, not a game lookup.
- Switching the feature, or the lookup cache, on or off bumps the generation: nothing stored under the other mode is used.
- **Grace period:** a write that was already running inside a game function when its slot was swapped is not bracketed.
  Sums are neither taken nor trusted during the first 10 s after the hooks go in (answers are probed as before
  meanwhile); every such call has long returned by then.

## How it works: Faster File Lists (round 3, section 4.2)

Measured: CAS LoadBlendGeometryCallback 0x005DA0C0 ("CAS/LoadBlendGeometries/ExpressionKeyList") asks ResourceMgr for
every key of type 0x0A037DDA; every one of ~300 packages walks its whole index (0x004AC9C0 + predicate 0x005949F0).
17% of the SimService-dominated hitch samples, 5.2% of all hitch samples in the old session.

### The game side (VERIFIED)

- **0x004B1AE0 ResourceMgr::GetKeyList** thiscall(vector* out, filter*, bool unique), ret 0xC; slot +0x20 of the base
  vtable (**0x00FB2DC0**, its only reference). `unique` (byte) false: `for each {db, prio} in [this+0x30, this+0x34)`
  (read once, no lock) `count += db->vfunc+0x30(out, filter)`; returns count. `unique` true: another list (+0xA0) and a
  set; not cached.
- **0x00736660 ResourceSystem::GetKeyList** (slot +0x20 of the derived vtable, **0x00FFE270**): calls 0x004B1AE0 directly,
  then, when the count and `out` are not 0, returns **0x004AFCD0(out)** (cdecl: sort + unique of the whole vector,
  returns its size).
- `out` = {begin, end, capacity, allocator}, 16-byte keys {instance lo, instance hi, type, group}. The databases only
  append: in place when end < capacity, else the vector insert **0x006D3810** (thiscall(pos, value*), ret 8, doubles the
  capacity), or resize 0x0045A5A0 then fill; they return the number appended.
- **The type filter** {vtable **0x00FD8248**, type}: its only virtual the databases use is +4 = **0x005949F0**
  (`mov eax,[esp+4]; mov edx,[eax+8]; xor eax,eax; cmp edx,[ecx+4]; sete al; ret 4`: key.type == this+4). 62 code
  sites build it.
- The read-only class's +0x30 is 0x00734550: with its key set present, a walk of that set (0x0072DE50); else open the
  file, walk the index (0x0072C930 -> index vfunc +0x1C = 0x004AC9C0), close. **The order of the keys inside one package
  therefore already differs between the two paths in the game.** A failed open returns 0 keys.

### The cache (features/resource_cache.cpp, `StartKeyLists`, `Hook_KeyListBase` / `Hook_KeyListDerived`, `EmulateKeyList`)

- Both slots through `SlotChain` (sites `KeyListBase` / `KeyListDerived`, layer `ResourceCache`; the profiler's "Key list"
  counter is outside). Start checks the base function's start and its non-unique loop (+0xF6), the vector insert (the
  CALL at +0xA4), the derived function (and that its first CALL reaches the base), the sort (its second CALL), the type
  filter's predicate and the read-only class's +0x30 against the studied bytes.
- Cached calls: `unique` false, an `out` vector, a filter whose vtable is the type filter's, no list change in progress.
  Everything else goes to the game.
- The cache runs the same loop itself. A read-only package whose keys of that type were captured under the current
  generation (and less than 60 s ago): its keys are appended as the game appends them and its return value added. Any
  other package, or no capture yet: the real call; for a read-only package the keys it appended are captured and kept
  when the generation did not move and the answer was sure (keys found, or an empty list with the key set present / the
  file open before and after). The derived hook then sorts and uniques exactly like 0x00736660.
- Same packages in the same order, same keys per package; only the order inside one package can be the other of the
  game's own two orders. Limits: 262,144 keys in total (4 MB; more: everything is dropped), 65,536 per list.
- Development check (1 call in N, as the lookup cache): the real calls run for every read-only package with a kept list
  and their keys are compared as a set (plus the return value); a difference turns the file list cache off for the
  session and is logged.

## How it works: Faster Texture Compression (C9)

### The game side (Steam 1.67.2; VERIFIED instruction by instruction)

**Drivers.** `0x006152F0` (DXT1) and `0x006154B0` (DXT5), `cdecl(Dst*, Src*)`, both `ret` with `eax = width & ~3` (no
caller reads it). `Dst` = {+0 first block, +4 width, +8 height, +0xC bytes per row of blocks}; `Src` = {+0 32-bit pixels
B,G,R,A, +0xC bytes per pixel row, +0x10 format}. The DXT5 driver does nothing unless `Src.format` is 0x3D or 0x3E; the
DXT1 driver never reads it. Blocks in raster order; full blocks use the fetch `0x00614000`, blocks of the last column
(width not a multiple of 4) or the last row use `0x00614C50(src, pitch, cols, rows)`. DXT1: colour block `0x006143F0(out,
1)`; DXT5: alpha block `0x00614150(alpha[16], out)` then `0x006143F0(out + 8, 0)`. The helpers have no other callers.

**Callers** (8 DXT1, 7 DXT5, several threads): `0x005FBBB0` (CAS composited textures, switch on the format),
`0x00618600` / `0x00618930` ("Services/ImgData"), `0x009DC8D0` ("VideoRecording/SceneCaptureTexture"), `0x009DD250`
("SceneCaptureManager/PackedImage", a mip loop), `0x00ADE030` ("LotLODCreator/LODTexture", lot impostor textures, DXT5),
`0x00C25C30` ("TerrainBuilder/PackedNormalMap", a mip loop, DXT5), `0x00D523E0` ("UI/ThumbnailManager/PackedImage", mip
loop, DXT1), `0x00D52FF0`, `0x00D653A0` (mip loops). Sizes are whatever those images are (mips down to 1x1); the
profiler's "pixels" extra measures them.

**Colour block, step by step** (a 16-byte-aligned object: axis at +0, 16 pixels at +0x10, endpoints at +0x110 / +0x120):
1. *Fetch.* Each pixel becomes {R/256, G/256, B/256, L} with L = 0.59f·G' + (0.11f·B' + 0.3f·R') (single precision, that
   order). The full fetch gets x/256 with float bit tricks (mask + or + add; R's bit 7 through the exponent), the partial
   fetch with `cvtsi2ss` × 1/256: both exact, identical. Partial blocks repeat each row's last pixel to 4 columns, then
   the last row to 4 rows.
2. *Moments* (`0x00615100`): sums over the 16 pixels in order of x, x², x·L (4 lanes); mean = sum/16; var = sum(x²)/16 −
   mean². total = (varL + varG) + (varB + varR).
3. *Axis.* total < 1e-5 → "solid": axis {1,1,1,0}, both endpoints = mean. Else if varL > 1e-5: axis = cov(RGB, L)
   (= sum(x·L)/16 − meanL·mean), lane 3 masked, × `rsqrtss` of (covB² + covR²) + covG² (no Newton step). Else
   (flat luma, colours vary): `0x00614DD0`, power iteration: covariance matrix of R,G,B (off-diagonals from the sums of
   G·R, B·G, R·B, sums/256), scaled by max(1, NR(rsqrtps(|var|²))), start = the row of the largest variance (ties go to
   G then B), 16 iterations of w = M·v, stop with the luma weights {0.3, 0.59, 0.11} when |w| ≤ 2^-23 in all three
   lanes, else v = w × (rsqrtps + 2 Newton steps).
4. *Endpoints.* For each pixel t = (dB + dR) + dG with d = (x − mean)·axis; mn = `minps`(0, t), mx = `maxps`(0, t);
   ep0 = `rcpss`(Σmx) · Σ(−mn·x), ep1 = `rcpss`(−Σmn) · Σ(mx·x) (4 lanes; the two weights are equal in exact arithmetic,
   the game uses them crossed). If (ddB + ddR) + (ddL + ddG) < 0.004444 (ep1 − ep0 squared), both move apart by
   (ep1 − ep0) · `rsqrtss`(d²) · (1/31).
5. *565.* channel · 248/255 (G: 252/255) + 1.5·2^18 (G: 1.5·2^17), the float's bits minus the magic = the rounded
   integer; clamp to 0..31 / 0..63 with a byte trick. Equal colours → {c, c, 0, 0}.
6. *Projection range.* Both colours decoded (× 1/31, 1/63) and projected: p = (x3 + x1) + (x2 + x0) with x = e·axis;
   lo = min, hi = max (lo = p0 unless p0 > p1). **x87:** `fld hi; fsub lo; fabs; fcomip 1e-5` → if 1e-5 > |hi − lo| then
   hi += 1 (the only x87 code; its precision is the thread's x87 control word: 24-bit on the Direct3D device thread,
   53-bit elsewhere). range = hi − lo; inv = 1/range (`divss`).
7. *Ordered dither.* strength d = clamp((0.125 − range)·16, 0, 1) by sign bits; pixel k adds dither[k]·d, dither = the 4x4
   Bayer matrix/16 − 0.5 at `0x00FE8238`.
8. *Indices.* v4 = (p − lo)·(inv·3) + dither·d, code = round(v4) via + 1.5·2^23, clamped 0..3; error4 += (code − v4)²
   in pixel order. DXT1 also: v3 with inv·2, 0..2, error3. DXT1 picks 3 colours when error4 > error3·2.25 (DXT5 never).
9. *Packing.* 4 colours: rank 0..3 → code 0, 2, 3, 1; colour0 must be > colour1, else swap and xor 0x5555. 3 colours:
   codes 1↔2 swapped; colour0 must be ≤ colour1, else swap and codes 0↔1. (The index assignment assumes p0 ≤ p1; when
   quantisation reverses them the game still does this: kept.)

**Alpha block** (`0x00614150`, MMX): extremes = 0 or 255. No extreme in the block: 8-alpha mode, a0 = max, a1 = min.
Otherwise 6-alpha mode: a0 = min over the non-zero values, a1 = max over the values that are neither 0 nor 255 (none:
a0 = 63, a1 = 192), extremes get codes 6 / 7. Code = min(K, (max(0, pmulhw((a − min)·32, round(K·4096/range))) + 1) >>
1), K = 7 or 5, then reversed in 8-alpha mode and remapped (rank 0 → 0, K → 1, others +1). **Game quirk kept:** the
alpha fetch of right-edge blocks (`0x00613F80`) has an empty column-fill loop, so rows are packed `cols` apart and
pixels get another pixel's alpha code (only images whose width is not a multiple of 4). Worse, for a 1..3-pixel block in
the last row (rows × cols < 4) its final fill (`rep movsd` from 16 bytes back) starts before the array and copies the
DXT5 driver's four locals that precede it (array at the driver's `esp+40h`): `[esp+30h]` = the block's source pointer
(stored at 0x0061561E just before the call), `[esp+34h]` = y, `[esp+38h]` = height, `[esp+3Ch]` = the destination row
padding. Those dwords go through the same saturations (words, then bytes) as alpha values. `FetchAlphaPartial` models
them exactly (`DriverLocals`); the fast path uses the scalar alpha translation when such a value is not a byte, and
recomputes the alpha half of blocks it hands to the game's function (whose one-block image has other locals). Found by
`tools/dxt_test` (edge sizes, 2026-09-29).

Constants used (all from `.rdata`, exact bit patterns in `features/dxt_codec.cpp`): 1e-5 (`0x00FE82C0`, `0x00F9D2C8`),
0.004444 (`0x00FE8288`), 1/31 (`0x00FE8284`), 1/63 (`0x00FAD528`), 1/16, 1/256, 248/255, 252/255, 1.5·2^18, 1.5·2^17,
1.5·2^23, 0.125, 16, 3, 2, 2.25, FLT_MAX, −1, 0.5, 1, 2^-23, the luma weights and the dither matrix.

### The replacement (`features/dxt_codec.{h,cpp}`, `features/fast_dxt.{h,cpp}`)

- `DxtCodec::Ref`: a literal translation (same instruction sequence with intrinsics, addresses in comments). It is the
  offline test oracle.
- `DxtCodec::Fast`: four blocks per SSE register ("structure of arrays": pixel k of blocks 0..3 in one register per
  channel). Blocks are gathered 4 at a time in raster order (a group can span rows); edge blocks get the game's
  replicated pixels; the pixel bytes are transposed and converted exactly (x/256 is exact either way). All sums,
  moments, projections and the index loop run on the four blocks at once; the DXT5 alpha block is SSE2 on all 16 pixels.
- **Why the bytes are identical** (the exactness argument): every float operation is the same IEEE operation on the
  same values with the same association order as the game's (e.g. (x3 + x1) + (x2 + x0), sums in pixel order);
  add, subtract, multiply and divide are correctly rounded, so for finite values the result does not depend on the SSE
  lane or on scalar vs packed; commutative operand swaps change nothing for finite values; `minps`/`maxps` keep the
  game's operand order (signed zeros); the approximate instructions `rcpss` / `rsqrtss` run per block as the same scalar
  instruction on the same input (the power iteration uses `rsqrtps` like the game); the x87 range test runs the game's
  exact x87 sequence (inline assembly) under the calling thread's control word; MXCSR (rounding, FTZ/DAZ) is the calling
  thread's in both. The one thing that could differ is NaN payloads (which NaN survives when two meet), so a block whose
  axis, endpoints, 1/range or errors are not finite is encoded by the game's own function (`GameBlock`: a one-block
  image at the block's pixels, which the game's drivers fetch exactly like the same block inside the whole image).
  Remaining assumptions, all checked in game: the translation itself (dev checks against the real function), and that
  the compiler keeps legacy SSE encodings (the file refuses `/arch:AVX`).
- **Hook:** the entries through `framework/entry_chain.h` (prologue `55 8B EC 83 E4 F0` to a trampoline, JMP written with
  every other thread suspended); layer 0 = Frame Profiler (times every call), layer 1 = this encoder; the trampoline is
  the game's function.
- **Checks:** the first 16 textures of every session (both builds) and in the development build 1 in N (default 8) are
  also encoded by the game into a scratch buffer and compared; a difference: `[FastDxt] Verification mismatch: ...`
  (block, both byte strings, the pixels), the game's bytes are used, and the feature turns itself off for the session.
- **Cost / expected speed (INFERRED, to be measured):** the game spends roughly 1,200-1,500 cycles per block (two MMX /
  SSE passes with store-forwarding, a divide, x87, a per-pixel horizontal dot product); the fast path about a quarter
  of that (the per-pixel work is vertical over four blocks; the scalar parts are 4 rcp/rsqrt, 1 x87 test and 1 divide
  per block). Expected 3-5x on the encoder, i.e. 44-49 ms hitches to about 10-15 ms. `tools/dxt_test` times it offline;
  the Developer card shows game vs Apex on the checked textures.

### Several cores (`DxtCodec::Parallel`, "Use several cores")

- **What:** an image with width x height >= 256 x 256 (dev slider) is cut into chunks of whole rows of blocks (about 4
  chunks per thread, at least 128 blocks each). The calling thread and up to N pool threads take chunks from an atomic
  counter until none is left; the hook returns only when every chunk is written (the game's callers expect the finished
  texture). N = min(logical processors of the process's affinity mask - 2, 6); 0 on 1-2 processor machines (then one
  core, as before). Smaller images, and a large image arriving while another thread's image has the pool, are encoded
  on the calling thread (the "busy" counter); nothing ever waits for the pool.
- **Why the bytes are the serial fast path's (exactness argument, VERIFIED offline):**
  1. *Per-block inputs are unchanged.* A chunk is encoded by `FastEncodeRows(dst, src, rowBegin, rowEnd)` with the
     WHOLE image's descriptors and absolute rows of blocks, so each block's Job is the one the serial loop builds: same
     source pointer (`src + pitch * 4 * row`, the address the serial walk reaches by repeated addition), cols / rows,
     output pointer, and the DXT5 driver locals that the edge-alpha quirk copies (the block's source pointer, y =
     absolute pixel row, height = the whole image's, padding = `dst.pitch - roundup4(width) * 4`). A split into
     sub-images would have changed y / height (and those blocks' alpha bytes): not done. Serial `Fast::EncodeDxt1/5`
     is now `FastEncodeRows(0, all rows)`, one code path.
  2. *Grouping does not matter.* Chunk ends change which blocks share a group of four (and add padding lanes); the fast
     path has no horizontal arithmetic between lanes (rcp / rsqrt / x87 test / power iteration run per lane, the
     push-apart branch only skips work, results are blended per lane), so a block's bytes do not depend on its lane or
     its neighbours. The flat-luma / solid counters count padding lanes, so those two statistics can differ slightly;
     blocks and "encoded by the game's function" do not.
  3. *Same FP state.* The caller reads its x87 control word (`fnstcw`: the range test's precision, 24-bit on the
     Direct3D device thread) and MXCSR (rounding, FTZ, DAZ); each worker saves its own, clears pending x87 exceptions,
     loads the caller's (`fldcw`, `ldmxcsr`, status flags cleared), reads them back (mismatch = counter, see below),
     encodes, and restores its own.
  4. *No shared mutable state:* each chunk writes disjoint output bytes (rows of blocks; row padding is never written);
     the source is only read; the fallback (`GameBlock`, the game's own encoder on a one-block image) keeps everything
     on its stack and is already called by the game from several threads.
  The offline test compares serial and parallel over the whole destination buffer (row padding included) on 23 sizes
  (1 x 1 to 2051 x 7, non-multiples of 4, 1-pixel strips, padded source and destination rows), 1 / 2 / 3 / 6 workers,
  row-sized and default chunks, under x87 53-bit, x87 24-bit, MXCSR round-to-zero + FTZ + DAZ and both together, plus
  the reference; a 3000-image stress of tiny splits and 4 threads encoding at once (with different FP states) cover the
  hand-off. Mutation check (2026-09-29): with the worker's FP-state load removed, the MXCSR runs differ in ~1.2 M blocks
  and the read-back counter fires in the x87 24-bit run (random data almost never flips the x87 test, so the read-back
  is the guard for the precision).
- **Pool:** created once (the first large image, or at `Start` / when the option is turned on), never destroyed (the
  threads sleep in `WaitForSingleObject` on their own auto-reset event and die with the process; nothing to tear down at
  exit or in `DllMain`). Hand-off: `state` = closed flag | threads checked in. The caller owns the pool (`inUse`), writes
  the task while closed, opens it, wakes k = min(N, chunks - 1) workers, takes chunks itself, closes, then waits (short
  spin, then the `done` event, re-checking the count) until no thread is checked in; a worker that wakes late (after
  the close, or during the next image) sees "closed" and leaves without reading the task, or joins the next image
  legitimately. Stack reservation 256 KB per worker (the game is 32-bit: 1.5 MB of address space for 6). Thread name
  "Apex DXT worker" (`SetThreadDescription` when present).
- **Priority: normal, on purpose.** The calling thread (render or loader thread) is blocked on the texture, so
  below-normal workers could only be delayed by the game's other normal-priority threads and make that wait longer.
  The work is bounded (one image, a few ms) and two logical processors stay out of the pool (one is the caller, which
  also encodes).
- **Checks unchanged:** the startup / 1-in-N checks encode with the game into scratch and with Apex (parallel when
  large) into the destination, then compare every row of blocks. A worker whose FP state did not read back as the
  caller's is counted, the whole image is encoded again on the calling thread (every block rewritten), and several
  cores stay off until the feature is started again (`[FastDxt] A DXT worker thread did not take ...` in the log).
- **Speed (offline, `tools/dxt_test`, this PC: 6 workers + caller, 2026-09-29):** 256 x 256 0.43 -> 0.09 ms (DXT1),
  1024 x 1024 7.3 -> 1.5 ms (DXT1) / 8.6 -> 1.55 ms (DXT5), 2048 x 2048 29 -> 5 ms / 34.5 -> 5.6 ms (about 5-6x on top
  of the fast path). In game the workers compete with the game's own threads; the Developer line "about X ms saved"
  (summed chunk time minus wall time; it slightly overstates when the cores slow each other down) measures it.

## How it works: Faster Cache Compression (C4)

### The game side (VERIFIED)

- **Stream vtable** `0x00FB9018` (constructor `0x004EBFD0`, `[stream+4]` = allocator): +0 destructor, **+4 write
  `0x004EC200`**, +8 read `0x004EC010`, +C `0x004EC080` (magic test). Slot +4 (`0x00FB901C`) is the write's only
  reference.
- **`0x004EC200` thiscall(src, size, dst, capacity, flags), ret 14h.** mode = 1 when flags & 2, 2 when flags & 0x10000,
  else 0. dst == 0 and flags & 1: returns ((size × 20) >> 4) + 32 (no work). Otherwise `0x004EC0A0(dst, capacity, src,
  size, allocator, mode)`; with dst == 0 it compresses without writing and returns the size (a "counting run").
  `capacity` is never read; the call never fails.
- **`0x004EC0A0`.** Header word 0x10FB (0x90FB and a 4-byte size when size ≥ 0x1000000), | 0x4000 and window 0x3FFF
  unless mode & 1 (window 0x1FFFF, no 0x4000); then the size, big-endian, 3 or 4 bytes. size ≤ 0x4000: `0x004EB750` with
  a 256-entry table (1 KB `_alloca`); larger: `0x004EBB90` with a 64K-entry table (**256 KB** from the allocator). Both
  also allocate the chain array (window + 1) × 4 bytes (**512 KB** for the 128 KB window), free both at the end. Returns
  payload + 2 + size bytes.
- **`0x004EB750` / `0x004EBB90`** (identical but for the hash): `memset(table, -1, ...)` **every call** (256 KB);
  hash of 3 bytes (`b0 ^ b1 ^ b2`, or `(b0 << 8 | b2) ^ (b1 << 4)`); chain[pos & window] = previous head. For every
  position: **every** candidate of the chain inside the window is tried (quick reject on the byte at the current best
  length, then a byte loop), best = largest (length − opcode bytes), stop only at a 1028-byte match. Opcode cost 2 when
  offset ≤ 1024 and length ≤ 10, 3 when offset ≤ 16384 and length ≤ 67, else 4. Matches never reach the last 4 bytes.
  mode 2 inserts only the first position of each match, otherwise every position. Literal runs of 4..112, stop opcode
  0xFC + 0..3. On repetitive data the chains hold thousands of candidates per position: that is the hitch.
- **Callers.** MemoryDB commit `0x0072B0A0` (the Sim / object compositor caches, terrain world caches): size query
  (flags 1), buffer of that size, write with **flags 2** (128 KB window); -1 → store uncompressed. Package writer
  `0x004A7030` ("ResourceLoad/PackedFile/CompressionRefpack", saves and writable caches, entries 0x32..16 MB, called from
  `0x004A82C0`): a size query, or **counting runs** (flags 2, no destination) of each registered compressor to choose one
  and size the buffer, then the write with flags 1, 0x10001 or 2 (by the package's mode) into that buffer; -1 → the
  resource is written uncompressed (`0x004A82C0` also drops streams whose ratio is above its threshold).
- **Decompressor `0x004EB3B0`** cdecl(dst, capacity, src, srcSize): header flags 0x8000 (4-byte sizes) and 0x100 (a
  compressed-size field to skip), ignores 0x4000; checks every literal and copy against the capacity and the source
  length, every match offset against the start of dst; needs the stop opcode; returns the header's size or 0. The only
  RefPack decoder in the exe (the stream read `0x004EC010`, magic `(word & 0x1FFF) == 0x10FB`). The official S3SS
  replaces this function; Apex never patches it (it only calls it to check its own streams).

### The replacement (`features/refpack_codec.{h,cpp}`, `features/fast_refpack.{h,cpp}`)

- **Stream format = the game's:** same header for the same size and flags, same window per flags (offsets ≤ 0x3FFF or
  0x1FFFF), the same four opcode forms, the same "matches stop 4 bytes before the end" rule, same literal runs and stop
  opcode. Any RefPack decoder, the game's included, reads it; the bytes differ from the game's.
- **Search:** 4-byte multiplicative hash, 64K heads + 64K chain links (512 KB per context, reused: positions are stored
  as base + index and older entries fall below the base, so the table is never cleared per call); up to 32 chain
  candidates per position (dev slider 4..256), stop at 96 bytes; the game's cost / gain rule; one step of lazy
  matching; all positions of short matches inserted, a sample of long ones. Match lengths compared 16 bytes at a time.
- **Memory:** a pool of 4 contexts (allocated on first use, never freed: 512 KB each plus their token buffers, about 3.5 MB); a 5th simultaneous call gets a
  temporary context. Check buffers up to 1 MB are kept per context.
- **Counting runs:** answered by the fast compressor too, and the thread remembers (source, size, flags, search depth,
  which compressor): the write that follows uses the same compressor, even if the feature was switched meanwhile, so a
  buffer sized by one compressor is never filled by the other; the fast compressor also writes it with the counted
  flags and depth, so the stream has exactly the counted size (the package writer counts with flags 2 but writes with
  the package's flags 1 / 0x10001 / 2; with the game's own compressor a smaller window could make the write longer than
  its buffer, INFERRED from `0x004A7030`, not observed). When the feature is switched off, its vtable layer stays until the last
  counting run is 2 s old (`FastRefPack::Tick` from the patch's `Update`).
- **Capacity:** unlike the game, the write never goes past `capacity` (when non-zero): a stream that does not fit returns
  -1 (the callers store the data uncompressed).
- **Checks:** the first 16 streams of each session (both builds) and in the development build 1 stream in 8 (default; every stream until 30/09, when its decode through S3SS's decoder was 19% of the compression hitches)
  are decompressed with the game's decoder (`RefPackDecompress`, else Apex's translation) and compared with the source.
  A difference: `[FastRefPack] Verification mismatch: ...`, the game's compressor writes that stream (when the
  destination can hold its bound, else -1) and the feature turns itself off for the session. Optional (dev): the game's
  compressor on 1 stream in N, counting only, for the size / time comparison.
- **Expected (INFERRED, to be measured with `tools/refpack_test` and the dev comparison):** no per-call 768 KB
  allocation or 256 KB clear, and a bounded search instead of the whole chain: 5-20x faster on repetitive data (where
  the hitches are), a few percent larger streams (the caches are size-capped, so slightly earlier evictions).
- **The counting run's stream, kept for its write (30/09):** measured in game (30/09, 13 sessions): "RefPack compress"
  was the dominant cause of 81 hitches (10.5 s over the median, max 368 ms), each a counting run and its write of the same
  stream (the CAS SimService / TextureCompositor caches: 4 calls of ~5.5 MB, ~330 ms, ~65 MB/s). The counting run now
  compresses into a buffer of its thread (`Held`, SizeBound bytes; buffers up to 8 MB kept per thread, at most 12 MB for all threads together since 30/09, larger ones freed after the write)
  with the source's CRC-32C (SSE4.2 `crc32`, 4 interleaved lanes, ~0.3 ms per 5 MB); the write of the same source, size,
  flags and depth copies it when the CRC still matches (else compresses again, as before; no SSE4.2: never copied). The
  write's checks (decompress and compare) are unchanged.
- **Large streams on several cores (30/09):** a stream over one piece of `kSegmentBytes` (128 KB) is compressed by the
  segmented compressor (`ParseSegment` / `SegmentEncoder`): each piece parsed on its own with the hash chains first filled
  from the window before it and no match past its end, then written in order (literal runs continue across pieces). The
  bytes depend only on the input, flags and depth, so counting run and write agree whatever thread parsed what, and a
  stream compressed on the calling thread alone (the pool busy with another thread's stream) is the same. The pool
  (`ParPool`, the texture encoder's hand-off protocol): `DxtCodec::Parallel::DefaultWorkers()` threads (min(processors -
  2, 6)), rounds of 2 pieces per thread, the caller parses too and writes each round in order. Memory: 512 KB per worker
  context and 14 token buffers of 350 KB (on first use, never freed; ~8 MB at most on 8+ processors). Offline
  (`tools/refpack_test`, 5.6 MB buffers, flags 2): stream size within ±0.13% of `Compress`; one thread 10-20% slower than
  `Compress` (the window fill per piece); 7 threads 3.8x faster than one (thread start included), same bytes; ~3000
  segmented streams round-tripped through the game's decoder translation, piece-boundary sizes included.
  **In game (30/09, build 0d1408ad, the same cache writes before and after, hitch frames only):** the ~21 MB writes
  (4 calls: 2 counting runs + their writes) 299 ms (49 cases) -> 101 ms (3); ~11 MB 121 -> 43 ms; ~5.5 MB 78 -> 24 ms
  (44 / 6 cases); worst compression-dominated hitch 295-395 ms -> 135 ms. The user: "realmente parece melhor". In game the
  pieces run ~2x faster than one thread (offline 3.8x): the game's own threads are busy at those moments.

## How it works: Faster Sim Building (FastCasSort, 30/09)

Found with the frame profiler's sampling run (30/09, 10 minutes, CAS and play): "CAS SimService" dominated 38 hitches
(~80 ms each, 3.0 s over the median); 83% of their render-thread samples were keyed "TS3W fn~005D1010", with the stack
0x005D38F8 in 84%. **Pitfall:** the sampler guesses a function's start from the int3 padding before it, and FUN_005d1960
follows FUN_005d1010's jump table with no padding, so its samples were booked to 005D1010. A read-only probe on 005D1010
(development build, removed again) showed it cheap (2566 calls, 84 ms in all, writing plain private memory, not
write-combined); 0x005D38F8 is the return address of `call 005D1960`.

- **The game side** (VERIFIED in full.asm and re\out\fn_005d1960.c): FUN_005d1960 "CAS/ModelBuilder/TriangleSortDataList",
  cdecl(u16* indices, u8* vertices, u32 indexCount, u32 vertexCount, u16 stride, u8 positionOffset), plain `ret`, its only
  call at 0x005D38F3 in FUN_005d3760 "CAS/ModelBuilder/FillDrawable" (per mesh part, when the part asks for it). For each
  triangle: its three positions (x, y, z, x) / w from signed 16-bit values (the packing of FUN_005d1010 case 1), two unit
  edge vectors (rsqrtps + two Newton steps `(1 - r r L)(0.5 r) + r`, constants 1.0 at 0x0107A538 and 0.5 at 0x00F9A5AC), a
  cross product C; then for every vertex v of the part (read at `v & 0xFFFF`) the unit vector from P0 and the sum
  `(z + y) + x` of its product with C, counted when above FLT_EPSILON (0x00FE3474, `comiss`/`jbe`). The triangle list is
  sorted by EASTL's list merge sort (0x005CC2D0, merge 0x005C9DE0: a node of the second half goes first only when its
  count is larger, unsigned: stable, descending) and the indices are written back. triangles x vertices steps of divps +
  rsqrtps: a 2013-vertex, 3000-triangle part is ~90 ms.
- **The replacement** (`features/cas_tri_sort.{h,cpp}` pure code, `features/fast_cas.{h,cpp}` the feature, entry chain site
  `CasTriSort` layer `FastCas`, GameAddr `CasTriSort` group `FastCasSort`): every vertex's (x, y, z) / w once (the same
  divps), then four vertices per SSE instruction with each lane doing the game's operations in the game's grouping
  (rsqrtps per lane = the same approximation as on the game's broadcast value), the triangles split over up to 6 worker
  threads when triangles x vertices >= 300,000 (the pool is the texture encoder's hand-off protocol; the caller's MXCSR
  copied to the workers), then `std::stable_sort` by count, descending, and the write-back. Allocation failures fall back
  to the game's function on the untouched indices.
- **Checks:** the first 16 calls of each session (both builds) and in the development build 1 in 16 afterwards run the
  game's own function on a copy of the indices and compare; a difference: `[FastCas] Result differs from the game's`, the
  game's result is used and the feature turns itself off for the session.
- **Offline** (`tools/cas_sort_test`, read-only, console only): 565 meshes (packed like the game, odd values: w = 0 or
  negative, repeated vertices, degenerate triangles, index counts not a multiple of 3, vertex counts over 65536), every
  count and every sorted list equal to the literal translation (`CasTriSort::Ref`). Time: 1030 x 1500 21 -> 1.5 ms,
  2013 x 3000 88 -> 2.5 ms, 3363 x 5000 242 -> 5.2 ms (8 threads, thread start included).

## How it works: record checksums (part of Faster Cache Compression, 30/09)

- **The game side** (VERIFIED in full.asm): FUN_004fa4c0 cdecl(bytes, length, crc, bool invert), plain `ret`: an MSB-first
  table CRC-32, one lookup per byte (`crc = (crc << 8) ^ T[(crc >> 24) ^ b]`), table 0x0114D330 = the standard 0x04C11DB7
  table (static data in the file), result inverted when the low byte of the fourth argument is set; nothing read when
  bytes + length wraps. Called 4 times, all from the checksum filter of the texture compositor's cache package (vtable
  0x00FE2594: verify 0x0072C580 on reads, write 0x0072C610; set up at 0x005BBB8B, 0x005BBF6B and by 0x0072C602). The
  loading RE (loadre\compositor.md) found it at 6-10% of a 490 ms cache eviction cascade.
- **The replacement** (`features/fast_crc.{h,cpp}`, entry chain site `RecordCrc` layer `FastCrc`, GameAddr `RecordCrc` +
  `RecordCrcTable`, group `FastRecordCrc`; started and stopped by the Faster Cache Compression patch, a failure only logs
  `[FastCrc] Not used`): slicing by 8 with tables derived from the game's own table read at start (T[k][i] = T[k-1][i]
  advanced by a zero byte), so the value is the same for every input.
- **Checks:** at start the table must be linear (T[0] = 0, every entry the XOR of its bits' entries) and 192 test buffers
  (0..70000 bytes, 8 alignments, seeds 0 / ~0 / random, both inversions) must give the game's own function's values; then
  the first 16 calls of each session (dev build: 1 in 64 afterwards) are compared with the game's; a difference logs
  `[FastCrc] Result differs`, the game's value is used and the part turns itself off. Offline (scratch test with the
  table read from TS3W.exe): 4850 cases, all equal to the byte-wise loop.

## How it works: Faster Memory Handling (FastMemory, 30/09)

- **The game side** (VERIFIED in full.asm and this PC's SysWOW64 ntdll; research notes loadre\memory.md): one EA PPMalloc
  general allocator (dlmalloc style) for every thread, global pointer 0x011CB864 (operator new 0x004E3F90 reads it), one
  CRITICAL_SECTION at allocator+0x4E8 (pointer at +0x4E4) taken by every Malloc / Free, created by
  InitializeCriticalSectionAndSpinCount(cs, 10) at 0x004E48D1. Windows' spin budget is SpinCount x 10 TSC ticks: 10 is
  ~24 ns (effectively no spin); the game's other critical sections get the default 2000 (~5 us). Blocks of 128 KB and
  more ([+0x494]) get their own VirtualAlloc (0x004E512A) and are freed with VirtualFree(base, 0, MEM_RELEASE) at
  0x004E5306 inside FreeInternal, lock held, result ignored. Other VirtualAlloc calls: new core 0x004E4E81 / 0x004E4EB4 /
  0x004E4EDB, core growth 0x004E5536. The core tail decommit (0x004E504C) and the core release (0x004E48AB) are not touched.
- **The patch** (`features/fast_memory.{h,cpp}`, GameAddr `AllocGlobal` + `AllocMmapFreeCall`, group `FastMemory`):
  - SetCriticalSectionSpinCount(cs, 2000) after checking `[a+0x4E4] == a+0x4E8`; Windows keeps the flag bits. Put back on
    Stop.
  - The release call (6 bytes, `FF 15 [VirtualFree slot]`) becomes `nop; call Stub_Free` (the CALL ends where the original
    did, so a thread still inside the stub when the bytes are written back returns to an instruction boundary). The stub
    queues the address (32 entries, SRW lock) and wakes a helper thread (above-normal priority) that releases the queue;
    queue full, stopping, or not a plain release: VirtualFree on the calling thread.
  - The five VirtualAlloc calls, found at start as `FF 15 [VirtualAlloc slot]` within [release - 0x600, release + 0x300)
    (exactly 5 or the feature stays off), become `nop; call Stub_Alloc`: the same call; when it fails, the queue is
    released on the spot and the call made once more, so a queued release can never make an allocation fail.
  - The import slots are found in TS3W's import table (kernel32 VirtualAlloc / VirtualFree) and called through, so another
    module's IAT hook is kept. Every write is `MemPatch::WriteCodeSuspended`; Stop restores the release first, empties the
    queue, then the allocation calls.
- **Why nothing changes:** the allocator's state and every value it reads are the same; the spin only changes how long a
  waiting thread spins before it sleeps; the address range of a freed big block goes back to Windows microseconds later.
- **Measure:** Developer > Profiler shows the spin (10 -> 2000), the releases done by the helper and any VirtualAlloc
  retry. A/B: the sampling run's "system code called from" rows 004E5935 / 004E5954 / 004E69E5 / 004E6A0B (allocator lock)
  and 004E530C (the release).

## How it works: the unused Vulkan driver (address space, 30/09)

- **Why:** TS3W is 32-bit (large-address-aware: 4 GB). The address-space study of 30/09 (session notes loadre\addrspace.md,
  from S3SS's memory statistics in the 10 crash logs of 29-30/09: 1618-2250 MB free, largest free block >= 1 GB, no
  out-of-memory crash) found AMD's 32-bit Vulkan driver `amdvlk32.dll` (85 MB image, at 0x0FB30000 in the low 2 GB the
  game heaps use) loaded into the game on a PC with an RTX and an AMD integrated GPU: DXVK asks the Vulkan loader for every
  GPU ("Found device: AMD Radeon(TM) Graphics" in TS3W_d3d9.log) though it renders on the RTX.
- **The change** (`features/vulkan_driver_guard.{h,cpp}`, always on, no menu row): in Apex's Direct3DCreate9 detour, before
  the first real call (DXVK loads vulkan-1.dll there), `VK_LOADER_DRIVERS_DISABLE=amd-vulkan32.json,amdvlk32.json` is set for
  this process only (Vulkan loader 1.3.234+; this PC 1.4.341), when vulkan-1.dll is not loaded yet, no
  VK_LOADER_DRIVERS_* / VK_DRIVER_FILES / VK_ICD_FILENAMES / VK_ADD_DRIVER_FILES is set, DXGI lists an AMD adapter and a
  non-AMD one, the adapter with the most dedicated memory is not AMD, and every AMD adapter has under half of its memory.
  Log: `[VulkanDriverGuard] ...` and `[D3D] Adapter N of M; AMD Vulkan driver in the game: loaded / not loaded`.
- **Effect:** ~85-100 MB of address space back (image + the driver's heaps); the D3D9 adapter count drops from 2 to 1 (the
  game creates its device on adapter 0, the RTX, either way). Nothing drawn changes.
- **Apex's own kept buffers (same day):** the RefPack counting run's per-thread buffer (`Held`) is still kept up to 8 MB per
  thread, but all threads together keep at most 12 MB (`kKeepHeldTotal`); above that a buffer is freed after its write.
  Output bytes unchanged.
- **Monitor:** `features/address_space.{h,cpp}` (development build): see docs/features/frame-profiler.md, "Address space".

## How it works: Spread New Objects Over Frames (C6)

> **History:** suspended in v1.8.0 (2026-09-29) because a review found that a node held past its frame could be freed
> while still linked (the node destructor does not unlink `+0x18`, AddNode re-pushes a linked node). The user's crash at
> the time (garbage EIP 0xB9497401) turned out to be a different bug. Brought back the same day as an experimental option
> (off by default) with the **node lifetime guard** below: the facts were re-read in the disassembly, and every node the
> feature keeps queued is protected by hooks on the node destructor, AddNode and the holder teardown.

Purpose (MEASURED, plan section 2.1): Scene::BeginFrame is the dominant cause of 31% of the 25-50 ms camera-moving
hitches and 26% of the 50 ms+ ones; the pending-node drain once processed 2224 nodes in 2.84 ms in one frame (usually
few). Inside it most of the time is the per-node update (materials resolving textures, i.e. C1's lookups).

### The game side (Steam 1.67.2; `research\engine_map\full.asm`, `dwords.txt`)

VERIFIED = read in the disassembly; INFERRED = deduction.

- **Pending holder** = `[scene+8]` (0x68 bytes, constructor 0x006E4530, allocated in 0x006EB610): `+0x04..+0x08` the slot
  array of its nodes, `+0x18` counter (nodes processed by the last drain; nothing counts the list itself), `+0x20` /
  `+0x24` the sentinel {next, prev} of a circular intrusive list, `+0x2C` the spatial tree. VERIFIED.
- **Scene node** (base constructor 0x006FD710, vtable 0x00FF9D00; refcounted: count at `+4` changed with `lock xadd`,
  vfunc +0 AddRef 0x005044C0, +4 Release 0x00692720, +8 deleting destructor): `+0x18` / `+0x1C` its pending link {next,
  prev}: **0** = processed by a drain, **self-loop** = not queued (constructor, RemoveNode), anything else = linked; `+0x30`
  its owner (the holder). VERIFIED.
- **Queueing:** 0x006FAC70 MarkDirty, 0x006FCA20 (a node's children), 0x006FCB10 (LOD band change) and 0x006FD9F0 SetOwner
  (vfunc +0x1C) all do `if (link.next == 0) { link = self-loop; if (owner) 0x006E42E0(owner, node); }`; 0x006E42E0 is a
  plain push_back at the tail. **Every push needs owner != 0** (children are queued on their own owner's list, only if
  they have one). SetOwner(0) self-loops a link that is 0 and never unlinks. VERIFIED.
- **AddNode 0x006E6480** (thiscall(node, group), ret 8; only caller the scene thunk 0x006E84B0): returns at once when
  `node->owner != 0`; otherwise takes a slot, AddRef, SetOwner(holder), and at 0x006E64EF pushes the link again when it is
  non-zero, **without unlinking it** (correct for a self-loop, and for the stale link the teardown leaves). VERIFIED.
- **RemoveNode 0x006E4920** (only when `node->owner == this`): clears the slot, **unlinks** a non-zero link and self-loops
  it (0x006E4984), leaves the spatial tree (0x006FB490), SetOwner(0,0,0), Release. VERIFIED.
- **Holder teardown 0x006E4DE0** (only caller 0x006E97F0, which frees the holder right after): destroys the spatial tree,
  then SetOwner(0,0,0) + Release on every slot **without unlinking**; the list dies with the holder, and a surviving node
  keeps a stale link into freed memory. VERIFIED.
- **Node base destructor 0x006FD930** (thiscall, ret): detaches its children (0x006FC860, which unlinks their `+0x20` and
  calls their vfunc +0x5C), leaves its spatial cell (0x00706200, only the cell's list at `+0x1EC`), owner = 0, unlinks its
  own `+0x20` (its parent's child list), destroys `+0x1F8` and stores vtable 0x00FA1B78 (the "destroyed node" vtable).
  **It never touches `+0x18`.** 0x006FAE00 only fills a local transform. The base vtable 0x00FF9D00 is written only by the
  base constructor and this destructor, so every node destructor ends in it (23 call / tail-jmp sites, the derived
  destructors). VERIFIED.
- **Does the game ever free a node that is still linked?** Not while its list is live: a node can only be pushed with an
  owner, the owner comes from AddNode (which AddRefs the slot), and the holder's reference is released only by
  RemoveNode (after the unlink) or by the teardown (whose list dies with the holder). The guarantee is structural, not
  "drained within the frame", so holding a node across frames does not break it. VERIFIED for every direct path;
  INFERRED that no other code calls SetOwner(0) on a linked node or drops the holder's reference (the indirect vfunc +0x1C
  calls with three zero arguments in the whole exe are RemoveNode, the teardown and a window class, 0x00588ED0).
- **The drain 0x006E4130**, thiscall(holder), ret, 0xD1 bytes: splices the whole list into a sentinel on its own stack
  and empties the holder's list (0x006E413E..0x006E4192); `[this+0x18] = 0`; then, **newest first**, while the local
  list is not empty: `l = local.prev` (re-read from the stack every iteration); unlink it; `l->prev = l->next = 0`;
  `node = l - 0x18`; `node->vfunc+0x48()` (the base class's is `ret`); `b = 0x006FB4B0(node, &aligned32)`;
  `0x006FAD70(node, b)` (= `owner+0x2C -> 0x00705B70(node, b)`; skips a node whose owner is 0; its only caller is the
  drain); `[this+0x18] += 1`. Nodes queued during the loop go to the holder's (now empty) list. VERIFIED.
- **Callers:** 0x006EBC49 Scene::BeginFrame (every frame), 0x006DF9B5, 0x006EDBC7 (a scene query that drains first so
  its answer is current), 0x006EF07D, 0x006F226B (a render-to-texture path, when asked), 0x006F3CF1. Each drains its own
  scene's holder (`[scene+8]`). VERIFIED.
- **Threads:** none of the list functions takes a lock, so the game must use the list from one thread (the render
  thread, which runs BeginFrame). INFERRED. Release is atomic, so a node's destructor may run on any thread.

### The copy with a budget (`features/scene_budget.cpp`)

- `Hook_SceneDrain` is layer 1 of `framework/call_chain.h` on the CALL 0x006EBC49 (the Frame Profiler's "Scene pending
  nodes" counter is layer 0). Start compares the **whole drain** (0xD1 bytes, only the two CALL rel32s wildcarded) with
  the Steam code, checks that its two CALLs sit at +0xAE / +0xB6 and reach `SceneNodeBounds` / `SceneNodeSpatial`, checks
  the heads of AddNode (the owner test), the destructor (up to the base vtable store) and the teardown, and refuses
  otherwise.
- Per BeginFrame (render thread): samples the camera eye (`LotLightingMotion::SampleCameraMoving`, the same eye read and
  300 ms rule as Lot Lighting While Moving, usable whether that feature is on or not). Camera still, or a node waited
  `maxDeferMs` -> the game's drain (everything). Moving -> `BudgetedDrain`: the game's loop instruction for instruction
  (same splice, same unlink order, same calls, same counter) plus a stop test before each node: at least 8 nodes, then
  stop at `nodesPerFrame` (512) nodes or `msPerFrame` (2.0 ms, QPC). The nodes not reached are spliced back at the
  **tail** of the holder's list (the game queues at the tail and the drain takes from the tail), in order, so they are
  processed first next frame. Only scenes drained every frame (within 100 ms) are budgeted; others drain fully. Holder
  slots (8) are reused only after 1 s without a drain; with none free the scene drains fully.
- The other five drain callers are untouched: they process the nodes left exactly as they process what the game queued
  after BeginFrame (the list is always well formed when Apex returns).
- The Frame Profiler reads `[this+0x18]` (nodes processed this frame) and `SceneBudget::TakeDrainNote()` ("deferred" =
  nodes left).

### The node lifetime guard

Every node `BudgetedDrain` leaves queued is recorded (`link -> holder`, an `unordered_map` under an SRW lock that is never
held across a game call; an atomic count gives the hooks a lock-free "nothing recorded" path). Three layers of
`framework/entry_chain.h` (layer `SceneBudget`) are installed before the drain hook and removed after it:

| Site | Entry | Prologue moved | What the hook does |
|---|---|---|---|
| `SceneNodeDtor` | 0x006FD930 (any thread) | `55 8B EC 83 E4 F0` | a recorded node whose link is neither 0 nor a self-loop, and whose neighbours point back at it, is unlinked and self-looped before the game destroys it; the record is dropped |
| `SceneAddNode` | 0x006E6480 | `56 8B 74 24 08` | for a node without owner (AddNode's only working case): a recorded node that is still linked is unlinked first (the game would push it over a live link); the record is dropped. A node with an owner is left alone (AddNode returns at once) |
| `SceneHolderTeardown` | 0x006E4DE0 | `53 55 56 57 8B F9` | the holder's records are dropped before its nodes are released (their links then point into a dying list, which the game leaves as it is) |

No branch in .text lands inside the moved bytes (engine_map `calls.tsv` / `jmps.tsv`). The invariant the records keep:
**a recorded node whose link is neither 0 nor a self-loop is in a live list** (its holder's, or a drain's local list on
the stack while that drain runs). A recorded node can only be queued on its owner's list; its owner changes only through
AddNode (record dropped) or the teardown (records dropped); drains leave links at 0. So the hooks never write into a
dead list. Records are replaced after every budgeted drain (the nodes left now) and dropped after every game drain that
goes through the hook; they are kept while a drain runs, so a recorded node destroyed inside a drain is unlinked from that
drain's local list, which both the game's loop and the copy re-read every iteration.

Development build checks (logged once; "stopped" = the game's drain runs until the game restarts):
- before each node of the budgeted copy: the tail's neighbours point back at it, its vtable is inside TS3W.exe `.rdata`,
  is not the destroyed-node vtable (read from the destructor's store at +0x8D, 0x00FA1B78 on Steam) and its +0x48 slot
  is inside `.text`; otherwise stopped;
- before each budgeted drain: the same for every recorded node of the holder that is still linked;
- a node found with the destroyed-node vtable but still well linked is taken out by writing only its neighbours (its
  freed memory is never written), never called, counted as "repaired" and logged once, without stopping.

Stop: `g_on` false (a thread still inside the drain hook runs the game's drain), the drain layer, then the three lifetime
layers are removed and the records cleared. The nodes still left are processed by the next BeginFrame: the same wait as
any node the game queues after BeginFrame.

Dev statistics (Developer > Performance, and the Off line in the log): drains (camera still / forced / budgeted), nodes
processed and left, largest backlog, and the guard: nodes recorded, **unlinked at destruction, unlinked before AddNode,
repaired** (each expected 0: each is a case the game's code was read not to have), records dropped at teardown
(expected after world or lot changes), recorded nodes destroyed on another thread, left nodes whose owner is not the
holder that drained them (expected 0).

## How it works: Faster Object Lookups (C8)

Purpose (MEASURED, plan section 2.2): `0x00C5FA60` is 13.6% of the samples of lot-lighting-dominated hitches (the hot
leaf), with `0x00C62D55` / `0x00C60D76` 16% on the stack; the lookup has 233 direct callers including the script natives
(simulation thread).

### The game side (Steam 1.67.2; VERIFIED)

- **0x00C62D40** ObjectById, thiscall(mgr, idLo, idHi, `int* visited`), ret 0xC: `r = 0x00C60D30(...)`; returns `r` only
  if `r->vfunc+0x40() == 1`. The third argument is a visit counter (not a flag): every one of the 233 callers passes 0
  (207 `push 0`; the other 26 push a register that is 0 on that path: `xor reg,reg` earlier in the function, or, at
  0x0086F162 / 0x00C89B03, a register just tested for zero; checked site by site 2026-09-29); a non-zero one is passed to
  the game untouched anyway.
- **0x00C60D30** walk, thiscall(mgr, idLo, idHi, visited), ret 0xC: id 0 -> 0; for each root of the vector
  `[mgr+0x9C, mgr+0xA0)` (size re-read every step): `0x00C5FA60(root, ...)`, first non-zero wins. `mgr` =
  `[0x011ECBC4]` (WorldManager) at most sites.
- **0x00C5FA60** search, cdecl(node, idLo, idHi, visited): node 0 -> 0; **id at +0x48 / +0x4C == key -> node** (any
  type); `++*visited` if given; if `vfunc+0x40() == 2`: for i < `vfunc+0x58()` (re-read): `search(vfunc+0x4C(&i))`, first
  non-zero wins. So the answer is the first node in depth-first order (roots in order, a node before its children,
  children in index order) whose id is the key, if it is of type 1.
- **The tree's classes:** the tree base constructor 0x00C71980 has exactly two callers, so every node is one of:
  - **Layer** (type 2, "Lot/ObjMgr - Layer", vtable 0x010641B0, ctor 0x00AA9370, 0xB8 bytes): children = a vector of
    node pointers at `+0xA0 / +0xA4` (+0x40 = 0x00B742C0 `mov eax,2; ret`; +0x58 = 0x00AA93C0 `(end - begin) >> 2`;
    +0x4C = 0x00AA9190 `i < count ? begin[i] : 0`). Its mutators are reached **only through its vtable** (no direct
    CALL, calls.tsv / datarefs.tsv): +0x44 AddChild 0x00AAA080 (child+0x10 = layer, AddRef, push_back; a child with a
    parent is removed from it first, a parentless one from the WorldManager roots 0x00C65A20), +0x50 RemoveChildById
    0x00AA9830, +0x54 RemoveChild 0x00AA9780 (erase, child+0x10 = 0, Release), +0x5C RemoveAll 0x00AA9920, +0x14 Clear
    0x00AA9430 (also the destructor 0x00AA9500), +0x60 SetId 0x00AA9020.
  - **Lot** (type 1, "Lot/ObjMgr - Lot", vtable 0x01065268, ctor 0x00AC19F0, 0x4C0 bytes): +0x40 = 0x00619EF0 `mov
    eax,1; ret`, +0x60 SetId 0x00AB3300. So "object lookup by ID" is in practice **the lot lookup by lot id**.
  - Both are created with `new` by the WorldManager factories (0x00C64420 CreateObject(type 1 / 2), 0x00C64630,
    0x00C65030, 0x00C65E00, 0x00C66310) and two script / load paths (0x00792CD0, 0x00D41630); no derived class.
- **Ids** are written only by the base SetId 0x00C6DDF0 (called by the two SetId overrides) and zeroed by the base
  constructor (the only `[+0x48]` / `[+0x4C]` pair writes in the lot (0x00AA0000-0x00AD0000) and WorldManager
  (0x00C5F000-0x00C73000) code ranges).
- **The roots vector** `mgr+0x9C..+0xA4` is written by eight non-virtual WorldManager methods (0x00C65A20, 0x00C65CF0,
  0x00C65E00, 0x00C66260, 0x00C66310, 0x00C66AB0, 0x00C6CF80, the constructor 0x00C671E0); Layers also load their
  children in 0x00AAA190. **So not all mutators can be hooked through vtables**; hooking eight direct-called methods by
  their entries (and proving there is no other writer) was judged not verifiable enough.
- Threads: render (lot lighting 0x00AD7620, camera 0x0096EAE0) and simulation (script natives 0x00784F50..0x00797A60).
  The walk takes no lock.

### The index (`features/object_index.cpp`): a validated cache, no mutator hooks

- **Key** (mgr, idLo, idHi) -> **path**: depth d (1..6), for each level k the node pointer, its vtable and its index
  (level 0 in the roots, level k in level k-1's children). 4096 entries x 84 bytes (344 KB, `VirtualAlloc` once, never
  freed), open addressing (32 probes), a table stamp for "empty everything"; SRW lock shared for lookups, exclusive for
  stores, **never held while game code runs**.
- **Store** (after the game's walk returned r != 0): the parent chain r, `[r+0x10]`, ... (AddChild's parent pointer, used
  only as a hint), the root index and each child index found by searching the vectors, each class checked by the bytes
  of its vtable functions (containers: +0x40 returns 2 and +0x4C / +0x58 are byte for byte the vector accessors above;
  the object: +0x40 returns 1), then validated once like a hit. Only found objects are stored (misses always walk).
- **Hit** (`Validate`): from mgr's live roots down: `idx[k] < size` and `vector[idx[k]] == ptr[k]`, the vtable of
  `ptr[k]` unchanged, the containers' ids != key (else the walk would stop there and the lookup return 0), the object's
  id == key; the entry younger than 2 s. That is what the game's walk reads to reach the node, so the walk reaches and
  returns it **unless an earlier node in walk order has the same id** (the residual assumption, INFERRED: lot ids are
  unique; two lots or a lot and a layer with one id would make the game's own answer order-dependent). Reads start from
  `mgr` and follow only pointers taken from vectors in use now, so only live objects are read (SEH-guarded against a
  race with a mutator on another thread; the game's walk has the same race unguarded).
- **Any failure** (path changed, class changed, id changed, a fault): the game's walk runs, its answer is stored again,
  and the **whole table restarts** (a structural change was seen; every other answer is proven again by a walk).
- **Checks (both builds):** the first 64 answers of each session and then 1 in 64 (dev slider; "every answer for 10 s")
  also run the game's walk; equal = counted; different with the path no longer valid = inconclusive; different with the
  path still valid = `[ObjectIndex] Verification mismatch: ...` (manager, id, the path, both answers), the feature turns
  itself off for the session and the game's answer is returned. Every checked lookup returns the game's answer.
- Hook: layer 3 `ObjectIndex` of `framework/entry_chain.h` on 0x00C62D40 (8-byte prologue to a trampoline); the Frame
  Profiler's "Object lookup" counter is layer 0 and adds "from index". Start compares the three bodies (lookup from byte
  8, walk, search; rel32s wildcarded) with the Steam code and checks the three CALLs (lookup +0x10 -> walk, walk +0x41 ->
  search, search +0x68 -> itself).
- Cost per answer (INFERRED): an SRW shared acquire, a hash probe, an 84-byte copy, about 4 + 5 x depth loads (depth 2-3
  for a lot); against a walk with 2-3 virtual calls per node over every lot and layer of the world.

## Files and functions

| File | Symbols | Role |
|---|---|---|
| `features/resource_cache.{h,cpp}` | `Start`, `Stop`, `Hook_FindProvider`, `Find`, `Recheck`, `Remember`, `RoReliable` / `ReadOnlyAboveReliable`, `BuildSnapshot`, `MaybeFingerprint`, `Verify`, `Hook_RegisterDb*`, `Hook_SetDbPriority`, `Hook_DbChanged`, `AcquireWatchers` / `ReleaseWatchers`, `CheckReadOnlyClass`, `TakeLookupNote`, `GetStats`, `StatusText`, `ReportLine`, `RenderDeveloperUI`; round 3: `StartMisses` / `StopMisses` / `UpdateMisses`, `kSpecs` + `EpochHook<I, N>` + `Hook_DpfWriteDirect`, `WriteBegin` / `WriteEnd`, `EnableEpochs` / `DisableEpochs`, `InstallClassHooks`, `EpochSum` / `StableEpochSum` / `RefreshSum`; `StartKeyLists` / `StopKeyLists`, `Hook_KeyListBase` / `Hook_KeyListDerived`, `EmulateKeyList`, `KlFind` / `KlStore`, `AppendKey`, `TakeKeyListNote` | the lookup cache, missing files + write epochs, the file list cache |
| `features/lot_lighting_motion.{h,cpp}` | `Start`, `Stop`, `Hook_LotLightBudget`, `SampleCamera`, `ReadEye`, `ParseCamera` (`ParseRootGetter` / `ParseCameraGetter` / `ParseEyeRead`), `UpdatePresentSampler` / `OnPresentSample`, `SetBudgetMs`, `CameraMoving`, `StatusText`, `RenderDeveloperUI`; `StartWallAo` / `StopWallAo`, `Hook_WallAoStep`, `FramePassUsed`, `RunPass`, `WallAoStatusText`, `RenderWallAoDeveloperUI` | the budget wrapper and the wall shading gate |
| `features/dxt_codec.{h,cpp}` | `Ref::EncodeDxt1/5`, `Ref::EncodeBlock`, `Fast::EncodeDxt1/5`, `FetchFull` / `FetchPartial` / `FetchAlphaPartial` / `Endpoints` / `PowerAxis` / `EncodeColor` / `EncodeAlpha` (the game's helpers), `EncodeColorGroup` / `EncodeAlphaFast` / `FlushGroup` (fast), `X87RangeBelowEps`, `CpuHasSse2` | the DXT algorithm, pure (also built by `tools/dxt_test`) |
| `features/fast_dxt.{h,cpp}` | `Start`, `Stop`, `Hook_Dxt1/5` -> `Encode`, `RunFast` (parallel or serial), `GameBlock` (fallback), `SetSeveralCores`, `Checked`, stats, `RenderDeveloperUI` | the hook, checks and counters |
| `features/refpack_codec.{h,cpp}` | `Compress` (fast), `Decompress` (0x004EB3B0 translated), `GameCompress` / `GameCore` (0x004EC0A0 + 0x004EB750 / 0x004EBB90 translated), `ParamsFor`, `SizeBound`, `Context` | the RefPack format, pure (also built by `tools/refpack_test`) |
| `features/fast_refpack.{h,cpp}` | `Start`, `Stop`, `Tick`, `Hook_StreamWrite`, the context pool, the per-thread counting-run pairing, `Check`, stats, `RenderDeveloperUI` | the hook, checks and counters |
| `tools/dxt_test/dxt_test.cpp`, `tools/refpack_test/refpack_test.cpp` | offline tests (console only, no files written) | build and run lines in their headers and below |
| `features/scene_budget.{h,cpp}` | `Start`, `Stop`, `Hook_SceneDrain`, `BudgetedDrain`, `Hook_NodeDtor`, `Hook_AddNode`, `Hook_HolderTeardown` (the node lifetime guard), `CheckRecords`, `NodeLooksAlive`, `LinkConsistent` (dev checks), `MatchAt` (the drain body and hook head checks), `TakeDrainNote`, `Set*` (dev tuning), `GetStats`, `StatusText`, `RenderDeveloperUI` | Spread New Objects Over Frames (C6) |
| `features/object_index.{h,cpp}` | `Start`, `Stop`, `Hook_ObjectById`, `Find` / `Store` (table), `Build` / `BuildRaw` (path of a found object), `Validate` / `ValidateRaw` (a hit), `ShapeOf` (class shapes), `CheckThisOne` / `RecordMismatch` (verification), `TakeLookupNote`, `GetStats`, `StatusText`, `RenderDeveloperUI` | Faster Object Lookups (C8) |
| `features/lot_lighting_motion.{h,cpp}` | `SampleCameraMoving`, `EnsureCamera` (the camera eye parse, once, any thread: Start, StartWallAo or the first SampleCameraMoving) | the camera eye sampler, shared by the lot budget, the wall shading gate and the scene node budget |
| `patches/performance_patches.cpp`, `patches/performance.h` | `ResourceLookupCachePatch`, `ResourceLookupMissesPatch`, `FileListCachePatch`, `LotLightingMotionPatch`, `WallShadingWhileMovingPatch`, `FastTextureCompressionPatch`, `FastCacheCompressionPatch`, `SceneNodeBudgetPatch`, `ObjectLookupIndexPatch`, `Performance::LotLightingBudgetMs` / `SetLotLightingBudgetMs` / `*Status` | the ApexPatch features, their registration and the menu's accessors |
| `framework/slot_chain.{h,cpp}` | `SlotChain::Install` / `Remove` / `Next` / `Installed` / `GameFunction`; sites incl. `RefPackCompress`, `WallAoStep`, `KeyListBase`, `KeyListDerived`; layers `Gate` (outermost), `FrameProfiler`, `ResourceCache`, `FastCompress` | layered vtable-slot hooks (profiler + cache, profiler + fast compressor, gate + profiler) |
| `framework/entry_chain.{h,cpp}` | `EntryChain::Install` / `Remove` / `Next` / `Installed` / `GameFunction` / `Original`; sites `DxtEncode1/5`, `DpfWriteDirect`, `ObjectById`, `SceneNodeDtor`, `SceneAddNode`, `SceneHolderTeardown`; layers `FrameProfiler`, `FastDxt`, `ResourceCache`, `ObjectIndex`, `SceneBudget` | layered entry hooks (trampoline + JMP written with all threads suspended) |
| `framework/call_chain.{h,cpp}` | `CallChain::Install` / `Remove` / `Next` / `Installed` / `CallAddress` / `GameFunction`; site `SceneDrain`, layers `FrameProfiler`, `SceneBudget` | layered hooks on one CALL instruction (rel32 written with all threads suspended) |
| `framework/memory_patch.{h,cpp}` | `MemPatch::WriteCodeSuspended` | code write with every other thread suspended |
| `framework/game_addresses.{h,cpp}` | ids `ResRegisterDb` .. `CameraGetter`, `RefPackDecompress`, `SceneBoundsCall` .. `SceneNodeSpatial`, `ObjectTreeWalk`, `ObjectTreeSearch`, groups `ResourceCache`, `LotLightingMotion`, `FastTextureCompression`, `FastCacheCompression`, `SceneNodeBudget`, `ObjectIndex` | addresses (fixed on Steam, signatures elsewhere) |
| `features/frame_profiler.cpp` | `Hook_FindProvider` (SlotChain::Next, `kXCacheHits`), `Hook_RefPackCompress` (SlotChain::Next), `DxtEncode` (EntryChain::Next), `Hook_SceneDrain` (CallChain::Next, `kXDeferred`), `Hook_ObjectById` (EntryChain::Next, `kXIndexHits`), `AttachSlots` / `DetachSlots` (`T_ResLookup`, `T_RefPackCompress`), `AttachTarget` / `DetachTarget` (`T_DxtEncode1/5`, `T_ObjectById`, `T_SceneDrain`), report lines | profiler side |
| `apex_gui.cpp` | `PerformancePage`, `PerformanceCard`, `FeatureSwitchRow`, Overview rows, Developer > Profiler "Performance" card, search part "Performance" | menu |

## Game addresses and patterns

Resolved through `framework/game_addresses.cpp` (fixed on Steam 1.67.2; masked signatures on other builds; all checked
with `research\port169\sigcheck.pl`: 140 of 140 ok on 2026-09-29, and each alternate matches once at the same place). Full table in
[../engine/game-versions.md](../engine/game-versions.md) section 6.

| Id | Steam | Kind |
|---|---|---|
| ResFindProvider / ResFindProviderSlot0/1 | 0x004AFFC0 / 0x00FB2DE0, 0x00FFE290 | Sig / SlotsOf(2) |
| ResRegisterDb / ResRegisterDbSlot | 0x004B2D00 / 0x00FB2DD4 | Sig / SlotsOf(1) |
| ResRegisterDbDerived / ResRegisterDbDerivedSlot | 0x00736A70 / 0x00FFE284 | Sig / SlotsOf(1) |
| ResSetDbPriority / Slot0/1 | 0x004B2EC0 / 0x00FB2DDC, 0x00FFE28C | Sig / SlotsOf(2) |
| ResDbChanged / Slot0/1 | 0x004B0960 / 0x00FB2DEC, 0x00FFE29C | Sig / SlotsOf(2) |
| ShadowedDbVtable | 0x00FFE078 | Sig (dword in the ctor 0x007342F0) |
| LotLightBudgetCall / LotLightBudget | 0x00ADB95D / 0x00ADB120 | Sig / Target (fallback Sig) |
| CameraRootCall / CameraGetterCall | 0x00C6D5BD / 0x00C6D5C4 | Sig (+0 / +7) |
| CameraRootGetter / CameraGetter | 0x006E8330 / 0x006E8400 | Target |
| DxtEncode1 / DxtEncode5 | 0x006152F0 / 0x006154B0 | Sig (entry) |
| RefPackCompress / RefPackCompressSlot | 0x004EC200 / 0x00FB901C | Sig / SlotsOf(1) |
| RefPackDecompress | 0x004EB3B0 | Sig: the CALL in the stream read 0x004EC010 (not the entry, which S3SS detours); optional |
| WallAoStep / WallAoStepSlot | 0x0068B810 / 0x00FF05B0 | Sig (entry; alternate at +0x21) / SlotsOf(1) |
| WallAoDriver | 0x00688920 | Sig (the whole body; alternate at +0x1B) |
| ResKeyList / ResKeyListSlot | 0x004B1AE0 / 0x00FB2DC0 | Sig (entry; alternate = the derived function's first CALL) / SlotsOf(1) |
| ResKeyListDerived / ResKeyListDerivedSlot | 0x00736660 / 0x00FFE270 | Sig (the whole body) / SlotsOf(1) |
| KeyTypeFilterVtable | 0x00FD8248 | Sig (dword stored by CAS 0x005DA0C0 next to the type 0x0A037DDA) |
| DpfVtable / DpfDerivedVtable / DdfVtable / PackedStreamVtable | 0x00FB2600 / 0x01048DA0 / 0x00FB2420 / 0x00FFD790 | Sig (dword stored by the constructor; DPF alternate: its destructor) |
| DpfWriteDirect | 0x004A7FC0 | Sig (entry) |
| SceneDrainCall / SceneDrain | 0x006EBC49 / 0x006E4130 | Sig / Target (fallback Sig) (existing profiler ids) |
| SceneBoundsCall / SceneNodeBounds | 0x006E41DE / 0x006FB4B0 | InRange(SceneDrain, 0xD1) / Target (fallback Sig) |
| SceneSpatialCall / SceneNodeSpatial | 0x006E41E6 / 0x006FAD70 | InRange(SceneDrain, 0xD1) / Target (fallback Sig) |
| SceneNodeDtor / SceneAddNode / SceneHolderTeardown | 0x006FD930 / 0x006E6480 / 0x006E4DE0 | Sig (entry; alternates at +0x11 / +5 / +6) |
| ObjectById | 0x00C62D40 | Sig (existing profiler id) |
| ObjectTreeWalk / ObjectTreeSearch | 0x00C60D30 / 0x00C5FA60 | Sig (alternates: the CALLs inside the lookup / the walk) |

Round 3 ids: `sigcheck.pl` 147 of 147 ok (every new primary signature matches once; the alternates too). After merging with C6 / C8 (v1.5.0): 153 of 153 ok. With the C6 lifetime hook ids (2026-09-29): 158 of 158 ok.

Run-time checks on every build (not signatures): the read-only class's OpenRecord / base OpenRecord / DeleteRecord bytes
(`CheckReadOnlyClass`); the budget CALL's target and the `D9` / `DD` after it; the getters' shapes (`A1 imm32 C3`,
`8B 41 disp8 C3`) and the eye read `0F 28 40 disp8`; the DXT entries' prologue `55 8B EC 83 E4 F0` (or the entry
chain's own JMP); the RefPack slot holding the write (or the slot chain's outer hook); the first textures / streams of
each session compared with the game; the whole scene drain body (0xD1 bytes) and its two CALLs; the heads of the scene
AddNode, node destructor and holder teardown (and their prologues, or the entry chain's JMP); the object lookup,
walk and search bodies and their three CALLs; the object lookup's prologue `8B 44 24 0C 8B 54 24 08` (or the entry
chain's JMP); the scene drain CALL reaching 0x006E4130 (or the call chain's hook); the tree classes' vtable functions
(bytes) before a path is stored; the first 64 object lookups answered from the index of each session compared with the
game's walk.

## Interactions

- **Frame Profiler (dev build):** see "Hooking and the Frame Profiler". The "Resource lookup" counter shows all calls;
  its per-frame text adds "% from cache", the hitch lines ", from cache N" (after "misses", so `agg.pl` still parses
  them: it splits counters on "; "), the report "Resource lookup cache: ..." and "Lot lighting while moving: ...". The
  profiler's "Lot room solve" calls = lot levels (corrected 2026-09-29; it was "rooms").
- **Official Sims3SettingsSetter:** its source was read only to list the game sites it patches; none is touched here.
  Its 51 byte patterns were matched against TS3W.exe: nothing overlaps FindProvider, the resource manager methods,
  0x00736A70, the budget call 0x00ADB95D / 0x00ADB120, the room solve 0x006A8BA0 or the camera read 0x00C6D5BD (its LSO
  detours WorldManager::Update at 0x00C6D570; the camera site is 0x4D bytes later and only read). Its RefPack patch is
  the decompressor 0x004EB3B0, its lighting quality patch hooks 0x006A0E00 / 0x006A4480 / 0x0069FD60 / 0x006A8D20 /
  0x006A8C88 / 0x006A8DE0 (none of ours). A literal scan of `Sims3SettingsSetter.asi`, `Sims3Performance.asi` and
  `MonoPatcher.asi` for the addresses and slots used here found nothing.
- **Sims3Performance 1.0.0-beta1** (its log): hooks 0x00EA3820 (isinst cache), 0x00D7FC60 / 0x00D74800 / 0x00D7F8B0 /
  0x00D80BC0 (simulator stages), 0x0059C5B0 (localized strings), 0x00EBBDE0 / 0x00EA8720 (Mono ehash), the allocator
  arenas, STBL layout, thread stacks, short waits; PackageIndexWarmup only reads the Mods / DCCache files into the OS
  cache (188 files). No resource-manager or lot-lighting site.
- **Night Lights:** it queues room relights (QueueRoom 0x006C7160 on the light tree levels: at world load, dusk, the
  "relight lots" button, lamp edits) and level_light_share re-gathers outdoor rooms; those relights are solved by this
  same budgeted lot lighting update. While the camera moves they now take more frames (the rooms resume where they
  stopped, which the game already does at its own 5 / 10 / 15 ms budgets); nothing in Night Lights waits for a relight to
  finish within a frame. The resource cache does not touch lighting.
- **Old combined build / older S3SSApex.asi:** Apex idles when they are loaded (existing detection); Smooth Streaming
  (combined build) detoured 0x00ADB120's entry, which the call-site redirect would pass through.
- **Frame Profiler and the compression features:** the profiler's "DXT encode" and "RefPack compress" counters are the
  outer layers of the entry chain / slot chain, so they time whichever encoder runs (the fast one when on). A texture
  or stream checked against the game is timed with both inside the counter (the dev check is slower on those calls).
  The profiler's Hooks table says "the fast encoder / compressor is inside".
- **Round 3 and the official Sims3SettingsSetter:** its source has none of the round 3 addresses (the AO step / driver /
  slot, GetKeyList and its slots, the database vtables and their methods, 0x004A7FC0); its lighting quality patch hooks
  0x006A0E00 / 0x006A4480 / 0x0069FD60 / 0x006A8D20 / 0x006A8C88 / 0x006A8DE0, none of which is the AO solver.
- **Wall shading gate and Lot Lighting While Moving:** independent features sharing the camera detection (and its
  per-frame Present sample). The budget cap makes the room solves resumable pieces; the gate removes the one piece that
  is not. The Frame Profiler's "Lot room solve" includes the AO passes that run (they happen inside it).
- **Night Lights and the gate:** Night Lights never resets the wall AO (only message 0x0486519D and level creation do);
  its room relights are unaffected.
- **Remember Missing Files and the file list cache** share the lookup cache's generation and package-list watchers
  (installed while either cache is on). The epoch hooks are installed only while both the lookup cache and Remember
  Missing Files are on.
- **Official Sims3SettingsSetter and compression:** its "RefPack decompressor" replaces the decoder 0x004EB3B0 (it only
  reads streams); the fast compressor's streams use the same format and are checked in game through whatever decoder
  is installed (the game's or S3SS's). Nothing in S3SS touches the compressor, the stream vtable or the DXT encoders
  (plan section 6, literal scan of the ASIs).
- **Frame Profiler and C6 / C8:** "Scene pending nodes" is layer 0 of the call chain on 0x006EBC49 and "Object lookup by
  ID" layer 0 of the entry chain on 0x00C62D40; with the features on the counters time the budgeted drain / the index
  (the hitch lines add ", deferred N" and ", from index N"). With the profiler's scene counter, `[this+0x18]` = the
  nodes processed this frame (fewer while the budget defers). Either order of install works.
- **Official Sims3SettingsSetter and C6 / C8:** its source (read only) patches none of 0x006EBC49, 0x006E4130,
  0x006FB4B0, 0x006FAD70, 0x006FD930, 0x006E6480, 0x006E4DE0 (the last three also absent as literals from the installed
  `Sims3SettingsSetter.asi`, `Sims3Performance.asi` and `MonoPatcher.asi`, 2026-09-29), 0x00C62D40, 0x00C60D30, 0x00C5FA60 or the Layer / Lot vtables (its 0x00C63015 is a JZ byte in
  the lot visibility metric, a different function). Its Lot Streaming "object streaming throttle" detours
  AddLotObjectsToScene 0x00AC1130 and spreads object additions over frames itself; both can be on: it limits how many
  objects are created per frame, C6 how many queued scene nodes are placed per frame.
- **Lot Lighting While Moving and C6:** both use the same camera eye sampler (`LotLightingMotion::SampleCameraMoving`,
  the eye read parsed once from WorldManager::Update's code); C6 samples it once per frame whether or not Lot Lighting
  While Moving is on.
- **Lot lighting and C8:** the lot lighting code is the main render-thread caller of the lookup (0x00AD7620); with the
  index its "Object lookup" time should drop, and so "Lot room solve" per level.

## Pitfalls and risks

- **A lot lighting budget with the camera still too (tried 30/09, reverted the same day, commits 25b3ca4 / its revert):**
  the steady budgets (15 / 5 ms) scaled to 8 ms with the camera still, the lamp boost, lots in their first 10 s after
  loading and the tool mode left alone. In game (7 min, much of it in Build mode): the 14-16 ms "Lot room solve" hitches
  fell from 12% to 1% of them, but the 8-12 ms ones stayed (the solve overshoots its budget by one step), and the user
  saw lamps moved in Build mode update their light more slowly (moving a lamp is not a switch, so no boost; the budget
  function's `+0x4D` flag is the lot thumbnail's forced quality, `0x00ADC180` from UI/ThumbnailManager, not a Build mode
  flag). Small gain, visible cost: do not bring it back without a real Build-mode or lamp-moved signal.
- **Stale answer after a silent file change (C1, INFERRED risk):** a read-only package whose file changes on disk
  without the watcher noticing, or a closed read-only package re-targeted with SetLocation (slot +0x2C, only allowed while
  closed; only seen at registration) would change what the game finds without a notice. Bounded by the 60 s entry age
  and caught by the development checks.
- **Transient open failure (C1, INFERRED risk):** if a read-only package without its key set fails to open during the
  game's own lookup (file locked by another program), the game answers with a lower package; that answer could be stored
  and served up to 60 s (the game's own resource cache keeps such a resource too). Since round 3 `Remember` refuses to
  store when a read-only package above the answer is closed with no key set after the lookup (the state a failed open
  leaves); only a racing successful open by another thread could hide it. The development checks would log it.
- **Many non-read-only packages above the answers (C1):** each answer probes them all; more than 32 above an answer =
  not stored. The Developer line "N not of the read-only class" shows the count; with `writable` Resource.cfg lines or
  many code-registered databases the gain shrinks.
- **The 60 s re-check** makes each distinct key cost one game lookup per minute (about 0.1 ms per second for 5000 keys).
- **Do not free the table** or move the SRW lock: threads may still be inside the hook after Stop.
- **Do not detour 0x004AFFC0's entry** (the slots are its only references; code bytes would also be seen by the
  game-address self-check) and do not swap the FindProvider slots outside `SlotChain` (the profiler and the cache would
  lose each other).
- **Lights settle later while moving (C7, by design):** the current lot's lights (and lots streaming in) finish over more
  frames while the camera moves; they catch up within ~300 ms of stopping plus the remaining work at full budget. The
  earlier Smooth Streaming "current lot while moving" cap was never tested with non-default values (removed-features.md).
- **Tool mode (C7):** budgets of 100 ms or more are never scaled.
- **DXT translation errors (C9):** the offline test compares the fast path with the translation, not with the game; the
  in-game checks (first 16 textures of every session, dev 1 in 64: each check runs the game encoder too, so a high rate makes rebuild frames slower in the dev build) compare with the real function. Any "Verification
  mismatch" line: keep the feature off and send the line (it has the block's pixels and both byte strings).
- **DXT and compiler flags (C9):** `dxt_codec.cpp` must be built with legacy SSE (`/arch:SSE2`, the x86 default; it
  refuses `/arch:AVX`) and without `/fp:fast` (it forces `float_control(precise)` and `fp_contract(off)`). Do not
  "optimise" its arithmetic order, replace `rcpss` / `rsqrtss` by divisions, use `rcpps` / `rsqrtps` on four blocks, or
  replace the x87 inline assembly: the output would stop being the game's.
- **DXT edge alpha (C9):** the game's partial-width alpha fetch is wrong (packed rows); it is kept on purpose (bit
  identity). A "fix" would change the output of every image whose width is not a multiple of 4.
- **Other game builds (C9):** the encoders' constants are assumed equal to Steam 1.67.2's; the per-session checks catch a
  difference on the first textures.
- **DXT several cores (C9):** never encode a chunk as a sub-image (own pointer / height): the DXT5 edge-alpha quirk reads
  y, height, the source pointer and the padding, so those blocks' alpha would change. Always `FastEncodeRows` with the
  whole image's descriptors. Never let a worker encode without the caller's x87 control word and MXCSR. Keep the call
  synchronous (the game uses the texture as soon as the function returns) and never wait for the pool (busy = serial).
- **DXT on hybrid CPUs (C9, UNVERIFIED):** `rcpss` / `rsqrtss` are approximations whose exact bits are CPU-specific. On a
  CPU with two core types (e.g. Intel P / E cores) they are assumed to give the same bits on both; this was already
  assumed by the serial path (Windows moves the calling thread between cores), workers only make it more frequent. A
  difference would show as a "Verification mismatch" (the feature then turns itself off); "Use several cores" off, or
  the dev worker slider at 0, removes the extra exposure.
- **RefPack stream sizes (C4):** streams are a few percent larger than the game's (bounded search). The package writer
  drops streams above its ratio threshold (stored uncompressed) and the memory caches are size-capped: slightly more
  memory / disk per cached item. The dev comparison and `tools/refpack_test` measure it.
- **RefPack counting-run pairing (C4):** the write that follows a counting run must use the same compressor (the buffer
  was sized by it); the pairing is per thread and matched on (source pointer, size); the fast write then uses the
  counting run's flags (the header then says 128 KB window where the package asked for 16 KB: every RefPack decoder
  accepts it, the game's ignores that bit). If the game ever wrote a counted
  stream from another thread, the write would use the current compressor and could return -1 (safe: stored
  uncompressed), never overflow (the fast compressor checks the capacity; the game's is only used when the capacity can
  hold its bound or the counting run was the game's).
- **Unloading (C4):** after switching the feature off the vtable layer stays up to 2 s; `FreeLibrary` of the ASI in that
  window would leave the slot pointing into unloaded code (ASI loaders never unload; noted for completeness).
- **Do not** swap the RefPack slot or hook the DXT entries outside `SlotChain` / `EntryChain` (the profiler and the fast
  paths would lose each other). The same for the scene drain CALL (`CallChain`) and the object lookup entry
  (`EntryChain`).
- **Wall shading gate must stay the outermost layer** of 0x00FF05B0: it recognises the driver by its own return address.
  A layer outside it would make every call "another caller" (passed through: safe, but the gate would do nothing).
- **Do not defer an AO pass without a bound:** lot load stage 20 waits for the first pass (the lot would stay unfinished,
  i.e. on its impostor, for as long as the camera moves) and the ThumbnailManager's lot capture for the refinement. The
  2 s wait per state and camera motion, then one pass per frame, keep both moving.
- **Do not defer calls with a budget of 100 ms or more:** the synchronous level solve (lot LOD switch, 60 s) and the tool
  mode (1000 ms, whose impostor pump runs the lot pass without Presents) expect the pass to run.
- **Absent entries and read-only packages (INFERRED):** a read-only package that failed to open during the game's lookup
  and was then opened by another thread before `Remember` looked at it would look reliable afterwards. Since the v1.5.0
  merge an absent answer is stored only when every read-only package of the list was reliable both **before** the game's
  lookup and after it: reliability only goes from unreliable (closed, no key set) to reliable (open, or key set built),
  so a package that was unreliable when the probe started keeps the answer out. Residual (INFERRED): a package that
  was reliable before only through its open file, was closed as idle during the probe, failed to reopen for it and was
  reopened by another thread before the check after it; the development checks would log it.
- **Write epochs cover only the traced classes and paths.** A write path missed by the study would leave a counted
  database's answers stale until the next generation or 60 s. The development checks (1 in 64, or every answer for
  10 s) compare with the game; any difference turns the cache off. Classes are all-or-nothing: a changed method keeps
  its class probed.
- **Epoch buckets:** 1024 buckets by pointer; writes to a database sharing a bucket re-check more entries (never fewer).
- **DDF record opens bump its epoch** (a record open can insert a newly appeared file): a mods folder with many loose
  files read often would keep re-checking the entries below it (probes, not game lookups).
- **File list order:** within one read-only package the kept list is in the order of the walk that captured it (key set
  or index); the game itself returns either order depending on whether the file is open. The known callers (CAS) sort
  and unique; a caller that depended on the order inside one package would already be inconsistent in the game.
- **File list memory:** up to 4 MB of keys in the 32-bit process; the Developer line shows lists and keys kept.
- **Pop-in while moving (C6, by design, INFERRED look):** a node placed later is not drawn (new) or culled at its old cell
  (moved) for the frames it waits. At the defaults (512 nodes / 2 ms, at most 500 ms wait) a lot that streams in with
  ~2000 nodes takes a few frames. If moving Sims or objects flicker at the edge of the screen while panning, lower the
  wait or raise the per-frame limits (Developer card) and report it.
- **Do not change the drain copy's order or the splice (C6):** the copy must stay the game's loop (newest first, unlink
  before the calls, the local sentinel re-read after every node because game code may unlink other nodes, the rest put
  back at the tail, where the drain takes from). Apex keeps no node pointer it dereferences outside the game's list:
  the records are keys (the link address), only read or written while the invariant says the link is in a live list.
- **Node lifetime (C6; the v1.8.0 suspension):** the node destructor 0x006FD930 never unlinks `+0x18` and AddNode
  0x006E6480 pushes a non-zero link without unlinking it, so a node freed or re-added while still linked would corrupt the
  list or make the next drain call freed memory. The game itself never does it for a live list (see "Does the game ever
  free a node that is still linked?"); the lifetime hooks make sure of it for every node Apex leaves queued. Do not remove
  them, and do not "fix" them into unconditional unlinks: after a holder teardown the game leaves nodes with stale links
  into freed memory, and unlinking those would write into freed memory. Only recorded nodes (whose list is known to be
  live) are ever unlinked.
- **Never write a destroyed node's memory (C6):** the "repaired" path takes a destroyed but still linked node out by
  writing only its neighbours; its own link is freed memory (the allocator may already use it).
- **Records of a torn-down holder (C6):** the teardown hook must drop them before the game releases the nodes; the
  holder's memory can be reused by a new holder at the same address (the waiting-slot key is only a key; the records are
  gone).
- **Threads (C6, INFERRED):** the list has no lock in the game; the budgeted copy runs where the game's drain runs. The
  destructor hook may run on any thread; it is counted ("destroyed on another thread") and still guarded by the SRW
  lock. If that counter grows together with a crash, report it.
- **Unproven consumer assumption (C6, INFERRED):** nothing was found that expects the pending list to be empty right
  after BeginFrame; if a crash or a missing object appears only with the feature on, turn it off and send the log.
- **Duplicate ids (C8, the residual assumption):** the index answers the remembered object as long as its path is valid;
  if a second node with the same id appeared earlier in the walk order (a lot re-created while the old one is still in
  the tree, or a Layer with a lot's id), the game's walk would answer the other one. Lot ids are unique by design
  (INFERRED); the first 64 + 1 in 64 checks would catch it ("Verification mismatch").
- **Table restarts (C8):** every path that stops validating restarts the whole table (conservative). If the Developer
  line shows restarts every few seconds while playing (lots added / removed often), the gain shrinks; a per-entry
  invalidation would then be worth it (open item).
- **Do not replace the validation with hooks on the Layer mutators alone (C8):** the WorldManager roots have eight
  direct-called writers and Layers load their children in 0x00AAA190; a hook-maintained index would miss those.
- **Do not free the object index table or move its SRW lock** (threads may still be inside the hook after Stop).

## Testing in game

Faster Game File Lookups (development build, Developer > Profiler > Performance):
1. Turn it on (SYSTEM > Performance). Log: `[SlotChain] ...: layer 2 installed` five times (layer numbers: 0 gate, 1 profiler, 2 cache, 3 fast compressor) and `[ResourceCache] On: ...`.
2. Load a save, pan and zoom around busy lots for a few minutes, enter CAS, Buy and Build, Edit Town, travel, save and
   load again. Expect: "Checks: N equal, 0 different"; "changes the hooks missed 0"; list changes counted at world load;
   "answered from memory" above ~90% after warm-up; "packages asked" a small number (1 + the non-read-only packages).
3. "Check every answer for 10 s" while panning and while loading a lot: still 0 different.
4. Frame Profiler on (either order): its Hooks table says "outer layer of the slot chain; the resource lookup cache is
   inside"; the "Resource lookup" row shows "% from cache" and its ms per frame drops from ~5 ms to well under 1 ms in the
   same scene. Turn the profiler off and on again: both keep working.
5. Any "Verification mismatch" line in `ApexRadiance_LOG.txt`: keep the feature off and send the line.
6. Only after several clean sessions: flip `enabledByDefault` to true.

Lot Lighting While Moving:
1. On by default; log `[LotLightingMotion] On: the call at 0x00adb95d ...` with the camera offsets (root 0x011d1860,
   +0x24, +0x60 on Steam).
2. Developer line: "Camera moving" while panning, "Camera still" 0.3 s after stopping; "last budget: game 15.00 ms ->
   3.00 ms" while moving over the current lot, "game 5.00 -> 1.00" for other lots.
3. Frame Profiler, camera test as before: the 16-50 ms moving hitches dominated by "Lot room solve" should drop; "Lot
   room solve" per hitch ~3 ms instead of ~15.
4. Visual: after stopping, the current lot's lights and a newly loaded lot's lights settle within about a second; try 1,
   3 and 6 ms. At night with Night Lights: lamps placed / removed still relight their rooms (a moment later while the
   camera moves).
5. Build / Buy mode and Edit Town still relight at once when still.

Wall Shading While Moving (on by default; development build for the counters):
1. Log: `[SlotChain] Wall AO solver step: layer 0 installed ...` and `[WallShading] On: wall shading step 0x0068b810 (slot
   0x00ff05b0) gated for calls from the solver driver 0x00688920 (return 0x0068894d) ...`.
2. Developer > Profiler > Performance while panning over a neighborhood that loads: "held while moving" grows, "passed
   through" stays small (only the synchronous solve at LOD switches), "first passes run after the wait" only on long
   pans.
3. Frame Profiler, camera test as in round 3: no moving hitch with "Lot room solve" over ~3.5 ms; the "Wall AO pass"
   counter shows passes only in still frames (or after 2 s), at most one per frame; the 54-108 ms lot-load hitches
   should become one pass per frame after the camera stops.
4. Visual: pan quickly to a new part of town and stop: the new lots appear (leave their impostor) at most ~2 s later than
   before; outdoor walls may look flat for a moment and get their soft shading within a few frames of stopping. Nothing
   should stay unshaded or unloaded after the camera has been still for a second.
5. Build mode, Edit Town (tool mode) and lot switches must behave exactly as before (those paths are passed through).

Remember Missing Files (experimental; turn on "Faster game file lookups" first):
1. Log: `[ResourceCache] Write epochs: DPF counted; DPF (derived) counted; DDF counted; packed stream counted` (any
   "probed (...)" names the class and why) and `[EntryChain] DPF direct record write (0x004a7fc0): layer 2 installed`.
2. Developer line "Missing files (on)": "answered from memory" should reach about a third of all lookups after warm-up;
   "not remembered (a read-only package could not answer for sure)" small. "Package list: ... (asked on every answer
   unless counted: N)" gives the counted databases.
3. "Write epochs: ... answers with no probe of them" should dominate the answers; "sums refreshed" grows slowly (about
   one wave per compositor / cache write); the average "packages asked" per answer should fall towards 0-2.
4. "Check every answer for 10 s" in CAS (outfit changes write the CAS caches), in Build / Buy (lot saves write the lot
   DPF) and right after saving the game: still 0 different. Any mismatch line: keep it off and send it (it says
   "(write epochs)" when the entry relied on them).
5. Frame Profiler: "Resource lookup" per hitch should drop again (round 3 estimate: 4.1 ms to about 1.5 ms per hitch
   frame); the hitch lines show ", absent from cache N".

Faster File Lists (experimental):
1. Log: `[FileListCache] On: GetKeyList 0x004b1ae0 / 0x00736660 answered for the key type filter 0x00fd8248 ...`.
2. Enter CAS, change outfits and hair of several Sims, load a household: the Developer line "package lists from memory"
   should dwarf "packages asked" after the first pass; "File list checks: N equal, 0 different".
3. Frame Profiler: the "Key list" counter's ms per call should drop sharply after the first call of each type; the CAS
   SimService hitches (`0x005DA175` on the stack in sampling runs) should shrink.
4. Any file list mismatch line in the log: keep it off and send the line.

Offline tests first (console only, no files written; x86 Native Tools Command Prompt for VS 2022, or after
`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"`):
```
cd /d C:\Users\luiz_\Desktop\S3SS-dev\S3SSApex\tools\dxt_test
cl /nologo /O2 /EHsc /std:c++20 /arch:SSE2 /fp:precise /I..\..\features dxt_test.cpp ..\..\features\dxt_codec.cpp /Fe:dxt_test.exe
dxt_test.exe                      (10 million blocks per format; --blocks N, --seed N, --bmp file.bmp; --parallel-only)
cd /d C:\Users\luiz_\Desktop\S3SS-dev\S3SSApex\tools\refpack_test
cl /nologo /O2 /EHsc /std:c++20 /I..\..\features refpack_test.cpp ..\..\features\refpack_codec.cpp /Fe:refpack_test.exe
refpack_test.exe                  (--quick; --file path for real data)
```
Expected: "RESULT: all blocks identical" (the several-cores lines: "0 different from serial, 0 from the reference;
counter differences 0, FP state mismatches 0", some encodes "on one core because the pool was busy" in the 4-thread
stress) and "RESULT: every round trip and stream check passed"; the timing lines and the size / speed table against the
game's compressor go into the report.

Faster Texture Compression (development build, Developer > Profiler > Performance):
1. Turn it on. Log: `[EntryChain] DXT1 encoder (0x006152f0): layer 1 installed ...` (twice) and `[FastDxt] On: ...`.
2. Play: load a save, enter CAS, change outfits, pan over lots at different distances, change the season / weather, take
   a screenshot or a video capture. Expect "Checks: N equal, 0 different" growing (1 texture in 8 plus the first 16).
3. "Check every texture for 30 s" in CAS and while panning: still 0 different. Note "checked textures: game X ms, Apex
   Y ms (Zx)".
4. Frame Profiler on (either order): its Hooks table says "outer layer of the entry chain ...; the fast encoder is
   inside"; the "DXT encode" per-hitch ms should drop by about the measured factor.
5. Visual: identical by construction; any difference would be a mismatch line in the log.
6. Several cores (on by default): the `[FastDxt] On:` line says "images from 256x256 split over this thread and up to N
   workers (N created)". Developer card: "Split textures" grows in CAS / lot views / terrain, "% by workers" well above
   0, "FP state mismatches 0", "about X ms saved". Repeat step 3 with it on: still 0 different. Compare the Frame
   Profiler's "DXT encode" per hitch with "Use several cores" off and on; try the worker slider at 2 / 4 / max and the
   size slider at 128 / 256 / 512 while panning (the game's own threads share the cores).
7. After several clean sessions: flip `enabledByDefault` to true (public build: only the first 16 textures are checked).

Faster Cache Compression (development build):
1. Turn it on. Log: `[SlotChain] RefPack stream write: layer 3 installed ...` and `[FastRefPack] On: ... the game's
   decoder 0x004eb3b0`.
2. Play as above plus save the game (the package writer's counting runs) and load it again. Expect "Checks: N equal, 0
   different" (1 stream in 8 is checked by default), "writes after our counting run" > 0 after a save, "did not fit" 0.
3. Set "Also run the game's compressor on 1 stream in N" to 4 for a few minutes: note "size +x%, y.yx faster" (this
   slows those calls down; set it back to 0).
4. Frame Profiler: "RefPack compress" per-hitch ms should drop. Load the saved game after turning the feature off again:
   it must load normally (the streams are standard RefPack).
5. After several clean sessions: flip `enabledByDefault` to true.

Spread New Objects Over Frames (development build, Developer > Profiler > Performance):
1. Turn it on (Performance card, row with the Experimental badge). Log: three `[EntryChain] scene node destructor
   (0x006fd930)` / `scene AddNode (0x006e6480)` / `scene holder teardown (0x006e4de0): layer 4 installed` lines, then
   `[CallChain] Scene::BeginFrame pending-node drain (CALL 0x006ebc49 -> 0x006e4130): layer 1 installed` and
   `[SceneBudget] On: ... node lifetime hooks on the destructor 0x006fd930, AddNode 0x006e6480, holder teardown 0x006e4de0;
   ... at most 512 nodes / 2.0 ms per frame, longest wait 500 ms; development checks on (destroyed-node vtable
   0x00fa1b78)`. With the Frame Profiler on (either order): its Hooks table says "outer layer of the call chain ...; the
   scene node budget is inside".
2. Camera still: "drains ... game's (camera still)" grows, "with a budget" does not.
3. Pan quickly over a neighborhood while lots stream in, travel, load a save and pan at once: "with a budget" grows,
   "frames left nodes" > 0 when a lot streams in, "largest backlog" in the hundreds / thousands, "game's (a node waited
   too long)" rare. Look for: objects appearing a frame late (expected, subtle), objects flickering or missing after the
   camera stops (NOT expected: turn the feature off and report), any crash (turn it off, send the log).
4. Frame Profiler, the same camera test with the feature off and on: the "Scene pending nodes" per-hitch ms should drop in
   the 25-50 ms moving hitches; the hitch lines show ", deferred N".
5. Build / Buy: place, move and delete objects while panning; Edit Town; CAS and back: everything placed, nothing left
   invisible. Try the Developer sliders (e.g. 64 nodes, 0.5 ms) to make deferral obvious, then back to the defaults.
6. Node lifetime (what got it suspended): with the sliders low (64 nodes, 0.5 ms, longest wait 2000 ms) pan while lots
   stream in AND delete objects / Sims leave / travel / go to CAS / load another save, all while moving. The Developer
   "Lifetime guard" line: "nodes recorded" goes up and down; "unlinked at destruction", "before AddNode" and "repaired"
   stay 0 (any other value: send the log, it has a `[SceneBudget]` warning naming the node); "dropped at teardown" > 0
   after a world change is normal; "left nodes not owned by their holder" should stay 0. The status must not read
   "Stopped: a safety check failed" (else the log has `[SceneBudget] Safety check failed: ...`). Turn it off: the Off
   line in the log sums the same counters.
7. After several clean sessions: flip `enabledByDefault` to true.

Faster Object Lookups (development build):
1. Turn it on. Log: `[EntryChain] object lookup by ID (0x00c62d40): layer 3 installed ...` and `[ObjectIndex] On: object
   lookup 0x00c62d40 (walk 0x00c60d30, search 0x00c5fa60) answered from a validated index of 4096 entries; checks: the
   first 64 answers, then 1 in 64`.
2. Load a save and play (lot lighting at night, Sims going to community lots, travel, Edit Town: add / move / delete a
   lot, buy a lot, save and load). Expect "Checks: N equal, 0 different", "classes recognised: 1 container, 1 object",
   "from the index" above ~90% of the lookups after warm-up, "path changed" small and "table restarts" rare.
3. "Check every answer for 10 s" during a lot change in Edit Town and while panning at night: still 0 different
   (inconclusive is fine: the tree changed during a check).
4. Frame Profiler (either order): "Object lookup by ID" per-frame ms drops, the hitch lines show ", from index N"; in
   lot-lighting-dominated hitches the lookup share should fall. Its Hooks table: "outer layer of the entry chain ...; the
   object lookup index is inside".
5. Any `[ObjectIndex] Verification mismatch` line: keep it off and send the line (it has the id, the path and both
   answers).
6. After several clean sessions: flip `enabledByDefault` to true.

## Open items

- Negative caching and per-database write epochs: done in round 3 (Remember Missing Files). Open: MemoryDB (needs an
  entry hook on PutResource 0x0072C350) and the ContentManager database (non-virtual install code) stay probed; a
  resolve-level cache at 0x007D8110 (key -> resolved key) would also remove the first failing lookup.
- Which databases the "not read-only" ones are in the user's game (round3.md section 8, question 1): the Developer line
  gives the count and how many are counted; a per-generation dump of index / priority / vtable would answer it fully.
- Wall shading: if the 2 s first-pass wait looks bad, make the pass resumable per wall (round3 L3) or lower the wait.
- Why the current lot relights continuously while the camera moves (plan section 10, question 2).
- How often the game registers / unregisters packages or changes priorities while playing (lot streaming, travel, CAS):
  each one empties the cache. The Developer line "list changes" shows it; if it is frequent, a targeted invalidation
  (only entries at or below the changed index, after probing the new package) would keep the rest.
- Texture compression, several cores: pick the defaults (256 x 256, min(logical processors - 2, 6) workers) from the
  in-game "ms saved" and the profiler; mip chains are many calls (each level its own image), so levels below the
  threshold stay serial.
- Texture compression: an AVX2 path (8 blocks per register) would need proof that the VEX forms of `rcpss` /
  `rsqrtss` give the same results as the legacy ones on the user's CPU; not done.
- Cache compression: the search depth (default 32) and the "nice length" (96) are chosen without data from the game;
  pick them from `tools/refpack_test`'s sweep and the dev comparison (size vs time).
- Cache compression: the package writer compresses each resource twice (counting run, then the write); caching the
  counted stream per thread and copying it on the write would halve its cost (needs the buffer ownership rules checked).
- Scene node budget (C6): the per-frame limits (512 nodes / 2 ms / 500 ms) are chosen without data; pick them from the
  profiler's "Scene pending nodes, nodes N, deferred D" in the moving hitches. Which node classes are expensive in
  vfunc +0x48 (plan section 10, question 3) is still open: if one node dominates (a whole lot's model), a node budget
  cannot split it.
- Scene node budget (C6): the three developer sliders are not saved; if the defaults need changing for everyone, move
  them to registered settings under an "Advanced" section.
- Object lookup index (C8): a per-entry invalidation (instead of the whole-table restart) if the Developer line shows
  frequent restarts; misses (not found) are never cached (the game's walk runs every time): count them first.
- Object lookup index (C8): the Layer mutators (AddChild / RemoveChild* / RemoveAll / Clear / SetId) could be hooked
  through their vtable slots as an extra "something changed" signal, and the eight WorldManager root writers through
  their entries; not done (the validation already covers removals; only an earlier duplicate id is not covered).

### Test005 all-floor priority (2026-10-01)

While LevelLightShare's installed full-detail-all-floors policy is active, PriorityHook applies the existing x4000 boost to all floors of the priority lot, including those above the camera. Disabling that policy keeps the original camera-floor x4000 / below x2000 rule. Other lots and zero native priority are unchanged. Native priority differences still remain within the boosted floors. The extra solve drain stays at 4 ms still / 1 ms moving, and configured LotLightingMotion budgets are unchanged. Ambient publication is coordinated separately; no FPS improvement is inferred from these policy checks.

## Release 2.5.3 UI
The single Performance card is split into four cards documented in docs/ui.md. Main switches remain visible, dependent controls retain their previous behavior, and existing translated descriptions remain available on hover. Experimental badges are removed by explicit user request; this labeling change does not establish additional gameplay validation.

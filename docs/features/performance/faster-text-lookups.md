# Faster Text Lookups

Every name and text the game shows (pie menus, tooltips, catalog names, skill and trait names) is looked up in each of
its string tables in turn, one table per package that has text. With thousands of custom content packages every lookup
probes thousands of tables, and a text no table has probes all of them. Faster Text Lookups keeps one index of the
tables the game has already loaded and starts the game's search at the table that answers, so the game returns the same
text after one probe.

## Status

| | |
|---|---|
| Availability | Released in 4.0.13, off by default (marked Experimental in the menu) |
| Default | Off |
| Menu | System > Performance > Files and objects > *Faster text lookups* |
| Configuration | `[patches.StringTableIndex]` in `ApexRadiance.toml` |
| Source | [`features/string_table_index.{h,cpp}`](../../../features/string_table_index.cpp), [`features/string_table_index_core.h`](../../../features/string_table_index_core.h), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) (`StringTableIndexPatch`) |

## The problem

The string service keeps one hash table per loaded string table (STBL) in a list at service + 0x48: the tables of the
game's language first, in resource priority order, then those of the alternate language. A list node is `{next, prev,
resource key (16 bytes), table}`; a table holds a hash map of 7,919 buckets at + 0x28 (bucket = low dword of the key %
7,919); an entry is `{next, ?, key low, key high, text}`.

GetString (0x0059C5B0, thiscall(service; u64 key), ret 8) walks the list from its first node and probes each loaded
table (0x0059AF50) until one has the key with a text that is not null and not empty, then formats that text and returns
the game's string. A key no table has is probed in every table before the "missing" path runs. The walk takes no lock.

## How Apex Radiance solves it

- **Index:** key -> the list node the walk would stop at (the first table in list order with a non-empty text). Open
  addressing, 12 bytes a slot, load factor at most 3/4, at most 32 MB. Built from the tables the game already loaded:
  no file is read and no mod file is hashed.
- **Hook:** the first 6 bytes of the walk (0x0059C5C2, `mov esi,[ebx+48h]; add ebx,48h`) jump to Apex, which does what
  the walk's first 16 bytes did, asks the index for the node to start from and enters the walk's loop test (walk +
  0x4C) with it. The winning node: the game probes that one table and goes on exactly as before (its formatting checks
  whether that node is the list's first, so the real node is handed over, not a copy of the text). The list's end: the
  game's own "missing" path. The list's first node: the game's full walk (no answer).
- **When the index is stale:** the list changes only in three functions, each wrapped with Detours: the clear
  (0x0059AF00), the load of one table into its node (0x0059B190, called by the rebuild and by the resource change
  notification 0x0059BFC0) and the rebuild (0x0059BDE0, at start and on a locale change). Any of them makes the index
  stale; while one runs no answer is given. A lookup that finds the index stale builds a piece of it (at most 2 ms)
  and takes the full walk. Each answer also checks that the list's first and last nodes are those the index was built
  from.
- **Layout check:** before each build step the probe's bucket count (`mov edi,7919`), its end offset and the walk's
  end compare are checked; a difference turns the index off for the session.
- **Self-check:** the first 256 answers after each build and 1 in 64 later ones are compared with a walk done like the
  game's (same rule, in Apex). A difference turns the index off for the session (logged).
- **Threads:** the index is read under a shared SRW lock and built under the exclusive one, both taken with Try: a
  lookup never waits, it takes the walk instead. No lock is held while game code runs.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster text lookups | `[patches.StringTableIndex] enabled` | bool | off | | Starts the game's string table walk at the table an index names |

The row is not part of the Overview's Performance group switch and not in the Performance part of profiles.

## Compatibility and interactions

- **Stands down** (stays off, logged): another mod changed GetString's entry (`83 EC 78 53 55 56 8B D9 57`) or the
  walk's bytes (for example a string cache that hooks GetString), or `Mods\Packages\CCStringMerge_Strings.package`
  exists (string tables already merged into one per language offline).
- **Naming the mod:** when the checked code differs, the stand-down decodes the patch there (jmp rel32, jmp rel8,
  jmp [abs32], push/ret, mov reg/jmp reg; up to 4 jumps through memory outside any module) and names the module file it
  lands in (file name only): "... was changed by <name> (<file>). To use Faster text lookups, <setting>, then restart the
  game". Known mods get their setting (`features/string_table_index_culprit.h`); an unknown module its file name; a jump
  outside every module "a patch in memory (no file), likely a loader or trainer". An in-place change without a jump (as
  *StblBucketLayoutTuning*) names a known mod that is loaded, marked "likely".
- **Setup** (`installer/ApexSetup.Core/Compat.cs`) reports the same before the game starts, when the switch is on in
  `ApexRadiance.toml`: CCStringMerge's package, `Sims3Performance.ini` with either string option on (a missing ini or key
  counts as on), else the module the last game's stand-down named while it is still in `Game\Bin`. Read only: it never
  changes the other mod's files.
- **Sims3Performance:** its *LocalizedStringCache* detours GetString's entry and its *StblBucketLayoutTuning* changes
  the string tables' hash layout; with either on, this switch stands down (the layout is also checked before every
  index build, so a later change turns the index off for the session). Turn both off to use this one:
  `LocalizedStringCache=0` and `StblBucketLayoutTuning=0` in the `[Patches]` section of `Sims3Performance.ini`
  (`Game\Bin`; names from the mod's beta 2.1 ini and readme).
- **Game build:** signatures (address group `StringTableIndex`), each unique in Steam 1.67.2 and retail 1.67.2
  `TS3W.exe`; EA app 1.69: unverified. All instructions the hook relies on are checked before anything is written.
- A second walk with the same rule (0x0059CB00, another lookup entry) is not indexed.

## Limitations

- The first lookups after a load or a package change take the full walk while the index is built in steps.
- Over 32 MB of slots the index is not built and every lookup takes the walk until the list changes.

## Technical reference

- Signatures: `StblGetString` (`83 EC 78 53 55 56 8B D9 57 89 5C 24 1C E8 ?? ?? ?? ?? 8B 73 48 83 C3 48`), `StblWalk`
  (the walk through its loop test and `mov edi,[esp+10h]`), `StblClear`, `StblAdd`, `StblRebuild` in
  `framework/game_addresses.cpp`.
- Logs: `[StringTableIndex] On at 0x0059c5c2 (list changes at ...)`, `[StringTableIndex] Index built (#n): N strings from
  T tables, K KB, M ms in S steps` (the first 8 builds, then every 100th), `[StringTableIndex] Off: hits ..., missing ...,
  game walks ..., checked ... (differed ...), ...` when it is turned off.
- Offline check: `tools/string_table_index_test/run.ps1 [-Exe <TS3W.exe>]` (index semantics on fake tables, list
  changes, verification, cap, 3,000 tables against the walk, signatures).

## Rejected approaches

- Merging string tables offline into one per language (what CCStringMerge does): changes the user's files, and the
  merged order can differ from the game's priority order. The index keeps the game's own order.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-faster-text-lookups.md)
- [History](../../history/performance-faster-text-lookups.md)

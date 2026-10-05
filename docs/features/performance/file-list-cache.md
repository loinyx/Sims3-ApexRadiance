# Faster File Lists

Fewer stutters when Sims load outfits and shapes. Create a Sim and Sim loading ask the game for the list of every file
of one type, and every package then walks its whole index. Faster File Lists keeps each read-only package's list per
type and hands it back on the next request. The lists the game receives contain the same keys from the same packages.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier; on by default since 2.5.5 |
| Default | On |
| Menu | System > Performance > Files and objects > *Faster file lists* |
| Configuration | `[patches.FileListCache]` in `ApexRadiance.toml` |
| Source | [`features/resource_cache.{h,cpp}`](../../../features/resource_cache.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

CAS `LoadBlendGeometryCallback` 0x005DA0C0 ("CAS/LoadBlendGeometries/ExpressionKeyList") asks the resource manager for
every key of type 0x0A037DDA. Every one of about 300 packages then walks its whole index (0x004AC9C0 with the predicate
0x005949F0). This is a measurable share of the hitches dominated by the CAS SimService.

## How Apex Radiance solves it

Apex Radiance hooks both GetKeyList vtable slots and runs the game's per-package loop itself for type-filter requests:

1. For a read-only package whose keys of that type were captured under the current generation and less than 60 s ago,
   append the kept keys as the game appends them and add its return value.
2. For any other package, or with no capture yet, make the real call. For a read-only package, keep the keys it
   appended when the generation did not move and the answer was sure (keys found, or an empty list with the key set
   present or the file open before and after).
3. For the ResourceSystem slot, sort and unique the result exactly like 0x00736660.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster file lists | `[patches.FileListCache] enabled` | bool | on | | Turns the file list cache on. Independent of *Faster game file lookups*. A missing key reads as on |

Verification uses the lookup cache's *Check 1 answer in N* control ([resource-lookup-cache.md](resource-lookup-cache.md)).

## Compatibility and interactions

- Shares the lookup cache's generation and package-list watchers (installed while either cache is on). Every package-list
  change and change notice empties both.
- **Frame Profiler:** its "Key list" counter is the outer layer of both slots and adds ", keys N" and ", packages from
  cache N" to hitch lines; the report adds "File list cache: ...".
- **Official Sims3SettingsSetter:** its source has none of GetKeyList or its slots.

## Limitations

- Within one read-only package the kept list is in the order of the walk that captured it (key set or index). The game
  itself returns either order depending on whether the file is open. The known callers (CAS) sort and unique; a caller
  that depended on the order inside one package would already be inconsistent in the game.
- Up to 4 MB of keys are kept in the 32-bit process (262,144 keys in total; more and everything is dropped), at most
  65,536 per list. The Developer line shows lists and keys kept.
- Only non-unique requests with an output vector and the type filter are cached; everything else goes to the game.

## Technical reference

### The game side (Steam 1.67.2)

- **0x004B1AE0 ResourceMgr::GetKeyList** thiscall(vector* out, filter*, bool unique), ret 0xC; slot +0x20 of the base
  vtable (0x00FB2DC0, its only reference). `unique` (byte) false: `for each {db, prio} in [this+0x30, this+0x34)` (read
  once, no lock) `count += db->vfunc+0x30(out, filter)`; returns count. `unique` true: another list (+0xA0) and a set;
  not cached.
- **0x00736660 ResourceSystem::GetKeyList** (slot +0x20 of the derived vtable, 0x00FFE270): calls 0x004B1AE0 directly,
  then, when the count and `out` are not 0, returns 0x004AFCD0(out) (cdecl: sort and unique of the whole vector,
  returns its size).
- `out` = {begin, end, capacity, allocator}, 16-byte keys {instance lo, instance hi, type, group}. The databases only
  append: in place when end < capacity, else through the vector insert 0x006D3810 (thiscall(pos, value*), ret 8,
  doubles the capacity), or resize 0x0045A5A0 then fill; they return the number appended.
- **The type filter** {vtable 0x00FD8248, type}: its only virtual the databases use is +4 = 0x005949F0
  (`mov eax,[esp+4]; mov edx,[eax+8]; xor eax,eax; cmp edx,[ecx+4]; sete al; ret 4`: key.type == this+4). 62 code sites
  build it.
- The read-only class's +0x30 is 0x00734550: with its key set present, a walk of that set (0x0072DE50); else open the
  file, walk the index (0x0072C930 -> index vfunc +0x1C = 0x004AC9C0), close. The order of the keys inside one package
  therefore already differs between the two paths in the game. A failed open returns 0 keys.

### The cache

- Both slots go through `SlotChain` (sites `KeyListBase` / `KeyListDerived`, layer `ResourceCache`; the profiler's "Key
  list" counter is outside).
- Start checks the base function's start and its non-unique loop (+0xF6), the vector insert (the CALL at +0xA4), the
  derived function (and that its first CALL reaches the base), the sort (its second CALL), the type filter's predicate
  and the read-only class's +0x30 against the studied bytes.
- Cached calls: `unique` false, an `out` vector, a filter whose vtable is the type filter's, no list change in progress.
- Same packages in the same order, same keys per package; only the order inside one package can be the other of the
  game's own two orders.
- **Verification** (1 call in N, as the lookup cache): the real calls run for every read-only package with a kept list
  and their keys are compared as a set (plus the return value). A difference turns the file list cache off for the
  session and is logged.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| ResKeyList / ResKeyListSlot | 0x004B1AE0 / 0x00FB2DC0 | Sig (entry; alternate = the derived function's first CALL) / SlotsOf(1) |
| ResKeyListDerived / ResKeyListDerivedSlot | 0x00736660 / 0x00FFE270 | Sig (the whole body) / SlotsOf(1) |
| KeyTypeFilterVtable | 0x00FD8248 | Sig (dword stored by CAS 0x005DA0C0 next to the type 0x0A037DDA) |

Group `FileListCache`.

### Source

`features/resource_cache.cpp`: `StartKeyLists` / `StopKeyLists`, `Hook_KeyListBase` / `Hook_KeyListDerived`,
`EmulateKeyList`, `KlFind` / `KlStore`, `AppendKey`, `TakeKeyListNote`.

Developer counters: "File list cache: calls (cacheable, passed to the game); package lists from memory, packages asked;
kept lists (keys), not kept" and "File list checks".

## Rejected approaches

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-file-list-cache.md)
- [History](../../history/performance-file-list-cache.md)

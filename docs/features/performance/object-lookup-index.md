# Faster Object Lookups

Fewer hitches when lot lights update, and less work for the game's scripts. When the game looks up a lot by its ID it
searches the whole world object tree. Faster Object Lookups remembers where the game found each lot and re-reads that
path in the live tree instead of searching again. The answer is the game's own.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier; on by default since 2.5.5 |
| Default | On |
| Menu | System > Performance > Files and objects > *Faster object lookups* |
| Configuration | `[patches.ObjectLookupIndex]` in `ApexRadiance.toml` |
| Source | [`features/object_index.{h,cpp}`](../../../features/object_index.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The lookup by ID (0x00C62D40) walks every lot and layer of the world depth first, with two to three virtual calls per
node. It has 233 direct callers, including the lot lighting code on the render thread and the script natives on the
simulation thread, and its search function is the hottest leaf in hitches dominated by lot lighting.

## How Apex Radiance solves it

1. **Store** the path the game's walk took to a found object: for each level, the node pointer, its vtable and its index
   in its parent's vector.
2. **Validate** on every answer: from the manager's live roots down, each stored index is in range and points at the
   stored node, each vtable is unchanged, no container on the path has the key as its ID, and the object's ID is the
   key. Entries older than 2 s are walked again.
3. **Fall back** on any failure: run the game's walk, store its answer, and restart the whole table (a structural change
   was seen).
4. **Check** the first 64 answers of each session and then 1 in 64 against the game's walk.

There are no hooks on the tree's mutators; the validation reads exactly what the game's walk reads to reach the node.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster object lookups | `[patches.ObjectLookupIndex] enabled` | bool | on | | Turns the index on. A missing key reads as on |
| Developer > *Check 1 answer in N against the game* | `[developer.controls.object_index] verify_every` | int | 64 | 0 to 1024 | After the first 64 answers, also run the game's walk on 1 in N |
| Developer > *Check every answer for 10 s* | (not saved) | action | | | |

## Compatibility and interactions

- **Lot lighting:** the lot lighting code is the main render-thread caller of the lookup (0x00AD7620), so with the index
  its "Object lookup" time drops, and with it "Lot room solve" per level.
- **Frame Profiler:** "Object lookup by ID" is layer 0 of the entry chain on 0x00C62D40 and adds ", from index N" to hitch
  lines (from `ObjectIndex::TakeLookupNote`). Either install order works. The report adds "Faster object lookups: ...".
- **Official Sims3SettingsSetter:** patches none of 0x00C62D40, 0x00C60D30, 0x00C5FA60 or the Layer / Lot vtables (its
  0x00C63015 is a JZ byte in the lot visibility metric, a different function).

## Limitations

- The index answers the remembered object as long as its path is valid. If a second node with the same ID appeared
  earlier in the walk order (a lot re-created while the old one is still in the tree, or a Layer with a lot's ID), the
  game's walk would answer the other one. Lot IDs are unique by design (inferred); the checks would catch it.
- Every path that stops validating restarts the whole table. Frequent lot changes reduce the gain.
- Lookups that find nothing are never cached; the game's walk runs every time.

## Technical reference

### The game side (Steam 1.67.2)

- **0x00C62D40** ObjectById, thiscall(mgr, idLo, idHi, `int* visited`), ret 0xC: `r = 0x00C60D30(...)`; returns `r`
  only if `r->vfunc+0x40() == 1`. The third argument is a visit counter (not a flag): every one of the 233 callers passes
  0 (207 `push 0`; the other 26 push a register that is 0 on that path: `xor reg,reg` earlier in the function, or, at
  0x0086F162 / 0x00C89B03, a register just tested for zero). A non-zero one is passed to the game untouched.
- **0x00C60D30** walk, thiscall(mgr, idLo, idHi, visited), ret 0xC: id 0 -> 0; for each root of the vector
  `[mgr+0x9C, mgr+0xA0)` (size re-read every step): 0x00C5FA60(root, ...), first non-zero wins. `mgr` = `[0x011ECBC4]`
  (WorldManager) at most sites.
- **0x00C5FA60** search, cdecl(node, idLo, idHi, visited): node 0 -> 0; id at +0x48 / +0x4C == key -> node (any type);
  `++*visited` if given; if `vfunc+0x40() == 2`: for i < `vfunc+0x58()` (re-read): `search(vfunc+0x4C(&i))`, first
  non-zero wins. So the answer is the first node in depth-first order (roots in order, a node before its children,
  children in index order) whose ID is the key, if it is of type 1.
- **The tree's classes:** the tree base constructor 0x00C71980 has exactly two callers, so every node is one of:
  - **Layer** (type 2, "Lot/ObjMgr - Layer", vtable 0x010641B0, constructor 0x00AA9370, 0xB8 bytes): children = a vector
    of node pointers at `+0xA0 / +0xA4` (+0x40 = 0x00B742C0 `mov eax,2; ret`; +0x58 = 0x00AA93C0 `(end - begin) >> 2`;
    +0x4C = 0x00AA9190 `i < count ? begin[i] : 0`). Its mutators are reached only through its vtable (no direct CALL):
    +0x44 AddChild 0x00AAA080 (child+0x10 = layer, AddRef, push_back; a child with a parent is removed from it first, a
    parentless one from the WorldManager roots 0x00C65A20), +0x50 RemoveChildById 0x00AA9830, +0x54 RemoveChild
    0x00AA9780 (erase, child+0x10 = 0, Release), +0x5C RemoveAll 0x00AA9920, +0x14 Clear 0x00AA9430 (also the destructor
    0x00AA9500), +0x60 SetId 0x00AA9020.
  - **Lot** (type 1, "Lot/ObjMgr - Lot", vtable 0x01065268, constructor 0x00AC19F0, 0x4C0 bytes): +0x40 = 0x00619EF0
    `mov eax,1; ret`, +0x60 SetId 0x00AB3300. So "object lookup by ID" is in practice the lot lookup by lot ID.
  - Both are created with `new` by the WorldManager factories (0x00C64420 CreateObject(type 1 / 2), 0x00C64630,
    0x00C65030, 0x00C65E00, 0x00C66310) and two script / load paths (0x00792CD0, 0x00D41630); there is no derived class.
- **IDs** are written only by the base SetId 0x00C6DDF0 (called by the two SetId overrides) and zeroed by the base
  constructor (the only `[+0x48]` / `[+0x4C]` pair writes in the lot (0x00AA0000-0x00AD0000) and WorldManager
  (0x00C5F000-0x00C73000) code ranges).
- **The roots vector** `mgr+0x9C..+0xA4` is written by eight non-virtual WorldManager methods (0x00C65A20, 0x00C65CF0,
  0x00C65E00, 0x00C66260, 0x00C66310, 0x00C66AB0, 0x00C6CF80, the constructor 0x00C671E0); Layers also load their
  children in 0x00AAA190. Not all mutators can be hooked through vtables.
- Threads: render (lot lighting 0x00AD7620, camera 0x0096EAE0) and simulation (script natives 0x00784F50..0x00797A60).
  The walk takes no lock.

### The index

- **Key** (mgr, idLo, idHi) -> **path**: depth d (1 to 6), for each level k the node pointer, its vtable and its index
  (level 0 in the roots, level k in level k−1's children). 4,096 entries x 84 bytes (344 KB, `VirtualAlloc` once, never
  freed), open addressing (32 probes), a table stamp for "empty everything"; SRW lock shared for lookups, exclusive for
  stores, never held while game code runs.
- **Store** (after the game's walk returned r != 0): the parent chain r, `[r+0x10]`, ... (AddChild's parent pointer,
  used only as a hint), the root index and each child index found by searching the vectors, each class checked by the
  bytes of its vtable functions (containers: +0x40 returns 2 and +0x4C / +0x58 are byte for byte the vector accessors
  above; the object: +0x40 returns 1), then validated once like a hit. Only found objects are stored.
- **Hit** (`Validate`): from mgr's live roots down: `idx[k] < size` and `vector[idx[k]] == ptr[k]`, the vtable of
  `ptr[k]` unchanged, the containers' IDs != key (else the walk would stop there and the lookup return 0), the object's
  ID == key, the entry younger than 2 s (`kMaxAgeMs`). Reads start from `mgr` and follow only pointers taken from vectors
  in use now, so only live objects are read (SEH-guarded against a race with a mutator on another thread; the game's
  walk has the same race unguarded).
- **Checks:** the first 64 answers of each session (`kFirstChecks`) and then 1 in N also run the game's walk. Equal =
  counted; different with the path no longer valid = inconclusive; different with the path still valid =
  `[ObjectIndex] Verification mismatch: ...` (manager, ID, the path, both answers), the feature turns itself off for the
  session and the game's answer is returned. Every checked lookup returns the game's answer.
- **Hook:** `EntryChain` site `ObjectById`, layer `ObjectIndex` (8-byte prologue `8B 44 24 0C 8B 54 24 08` to a
  trampoline). Start compares the three bodies (lookup from byte 8, walk, search; rel32s wildcarded) with the Steam code
  and checks the three CALLs (lookup +0x10 -> walk, walk +0x41 -> search, search +0x68 -> itself).
- **Cost per answer (inferred):** an SRW shared acquire, a hash probe, an 84-byte copy, about 4 + 5 x depth loads (depth
  2 to 3 for a lot), against a walk with 2 to 3 virtual calls per node over every lot and layer of the world.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| ObjectById | 0x00C62D40 | Sig (shared with the Frame Profiler) |
| ObjectTreeWalk / ObjectTreeSearch | 0x00C60D30 / 0x00C5FA60 | Sig (alternates: the CALLs inside the lookup / the walk) |

Group `ObjectIndex`.

### Source

`features/object_index.{h,cpp}`: `Start`, `Stop`, `Hook_ObjectById`, `Find` / `Store` (table), `Build` / `BuildRaw`
(path of a found object), `Validate` / `ValidateRaw` (a hit), `ShapeOf` (class shapes), `CheckThisOne` /
`RecordMismatch` (verification), `TakeLookupNote`, `GetStats`, `StatusText`, `RenderDeveloperUI`.

Developer card *Find objects faster*: lookups, from the index, game walks (not found), too old, path changed, passed
through, stored / not stored, entries, table restarts, container / object classes recognised, per-second rates and
"saved about X ms", average walk vs answer, checks (equal / different / inconclusive) and the last difference.

### Rules for maintainers

- Do not replace the validation with hooks on the Layer mutators alone: the WorldManager roots have eight direct-called
  writers and Layers load their children in 0x00AAA190; a hook-maintained index would miss those.
- Do not free the table or move its SRW lock: threads may still be inside the hook after Stop.
- Do not hook the lookup entry outside `EntryChain`.

## Rejected approaches

- An index maintained by hooks on every tree mutator: the eight direct-called root writers could not be proven to be
  the only ones. See [history](../../history/performance-object-lookup-index.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-object-lookup-index.md)
- [History](../../history/performance-object-lookup-index.md)

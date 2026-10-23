# Faster Object Lookups: validation

The feature is described in [features/performance/object-lookup-index.md](../features/performance/object-lookup-index.md).
It must return the game's answer for every lookup, walk again whenever a stored path stops validating, and turn itself
off on the first difference with a valid path.

## Automated tests

None offline. Every session checks the first 64 answers and then 1 in 64 against the game's walk.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-22 | `cdff27e` | None: no offline harness covers the scene object map index | Not run; each session compares the first 256 answers of each map with the game's walk | not run in game yet |
| 2026-09-29 | | Caller audit of 0x00C62D40 | All 233 callers pass `visited` = 0 (207 `push 0`, 26 registers that are 0 on that path), checked site by site | None |

## In-game test plan

1. **Expected log:** `[EntryChain] object lookup by ID (0x00c62d40): layer 3 installed ...` and `[ObjectIndex] On: object
   lookup 0x00c62d40 (walk 0x00c60d30, search 0x00c5fa60) answered from a validated index of 4096 entries; checks: the
   first 64 answers, then 1 in 64`.
2. Load a save and play (lot lighting at night, Sims going to community lots, travel, Edit Town: add / move / delete a
   lot, buy a lot, save and load). **Expected:** "Checks: N equal, 0 different", "classes recognised: 1 container, 1
   object", "from the index" above about 90% of the lookups after warm-up, "path changed" small and "table restarts"
   rare.
3. *Check every answer for 10 s* during a lot change in Edit Town and while panning at night. **Expected:** still 0
   different (inconclusive is fine: the tree changed during a check).
4. Frame Profiler (either order). **Expected:** "Object lookup by ID" per-frame ms drops, hitch lines show ", from index
   N"; in lot-lighting-dominated hitches the lookup share falls. Its Hooks table: "outer layer of the entry chain ...;
   the object lookup index is inside".
5. **Expected:** no `[ObjectIndex] Verification mismatch` line.
6. Scene object map index. **Expected log:** `[RenderObjectMap] On: the scene's lookups by object ID (find 0x006fdfb0)
   answer from an index kept by its insert 0x006fe1f0 and erase 0x006fdf40; the first 256 answers of each map are
   checked against the game`, then `[RenderObjectMap] Indexed the scene map ...` once the world is loaded. The status
   line ends with `Scene object index: ... checked against the game, all equal`; no `An answer differed` error and no
   `left to the game` warning.

## Confirmed in game

- Nothing recorded beyond the default-on release in 2.5.5.

## Open checks

- A per-entry invalidation instead of the whole-table restart, if the Developer line shows frequent restarts.
- Misses (not found) are never cached; count them first.
- The Layer mutators (AddChild / RemoveChild* / RemoveAll / Clear / SetId) could be hooked through their vtable slots as
  an extra "something changed" signal, and the eight WorldManager root writers through their entries; not done (the
  validation already covers removals; only an earlier duplicate ID is not covered).
- Per-thread counters and the 16 recent answers per thread (`e4fb59a`): "from the index" share and "Object lookup by ID"
  time in a room-solve frame against the previous build; still no `Verification mismatch`. The totals on the Developer
  card lag by up to 256 counts per thread, by design.
- Object service ID index (2.7.0): the Off line's checked answers with no "Verification" or turn-off message in a long
  session; its effect on hitches when lot lights update.
- Scene object map index (`cdff27e`): step 6 in a long session with travel, Edit Town lot changes, save and load; how
  many maps are indexed and how often a table is built again (the Off line's *tables built*); the Frame Profiler's
  sampled share of 0x006FDFB0 on the render thread (2.8% before) and "Lot room solve" time against the previous build.
  Not yet run in game.

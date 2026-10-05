# Faster Object Lookups: validation

The feature is described in [features/performance/object-lookup-index.md](../features/performance/object-lookup-index.md).
It must return the game's answer for every lookup, walk again whenever a stored path stops validating, and turn itself
off on the first difference with a valid path.

## Automated tests

None offline. Every session checks the first 64 answers and then 1 in 64 against the game's walk.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
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

## Confirmed in game

- Nothing recorded beyond the default-on release in 2.5.5.

## Open checks

- A per-entry invalidation instead of the whole-table restart, if the Developer line shows frequent restarts.
- Misses (not found) are never cached; count them first.
- The Layer mutators (AddChild / RemoveChild* / RemoveAll / Clear / SetId) could be hooked through their vtable slots as
  an extra "something changed" signal, and the eight WorldManager root writers through their entries; not done (the
  validation already covers removals; only an earlier duplicate ID is not covered).

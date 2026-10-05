# Faster Game File Lookups: validation

The feature is described in [features/performance/resource-lookup-cache.md](../features/performance/resource-lookup-cache.md).
It must:

- Return exactly the (package, priority) the game's own lookup would return, for every key.
- Empty its table on every package-list change and change notice.
- Never store an answer computed across a list change or with an unreliable read-only package above it.
- Turn itself off for the session on the first verified difference.

## Automated tests

None offline. Verification runs in game: the developer checks compare 1 answer in N (and every answer for 10 s on
demand) with the game's lookup.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No recorded in-game check results | |

## In-game test plan

Developer mode, Developer > Performance > *File searches and remembered answers*:

1. Turn the switch on. **Expected:** log `[SlotChain] ...: layer 2 installed` five times (layers 0 gate, 1 profiler,
   2 cache, 3 fast compressor) and `[ResourceCache] On: ...`.
2. Load a save, pan and zoom around busy lots for a few minutes, enter CAS, Buy and Build, Edit Town, travel, save and
   load again. **Expected:** "Checks: N equal, 0 different"; "changes the hooks missed 0"; list changes counted at world
   load; "answered from memory" above about 90% after warm-up; "packages asked" small (1 + the non-read-only packages).
3. *Check every answer for 10 s* while panning and while loading a lot. **Expected:** still 0 different.
4. Frame Profiler on (either order). **Expected:** its Hooks table says "outer layer of the slot chain; the resource
   lookup cache is inside"; the "Resource lookup" row shows "% from cache" and its ms per frame drops from about 5 ms
   to well under 1 ms in the same scene. Turning the profiler off and on keeps both working.
5. **Expected:** no "Verification mismatch" line in `ApexRadiance_LOG.txt`. If one appears, keep the feature off and
   report the line.

## Confirmed in game

- Nothing recorded beyond the default-on release in 2.5.5.

## Open checks

- Which databases are not of the read-only class in a typical game: the Developer line gives the count and how many are
  counted; a per-generation dump of index / priority / vtable would answer it fully.
- How often the game registers / unregisters packages or changes priorities while playing (lot streaming, travel, CAS):
  each one empties the cache. If frequent, a targeted invalidation (only entries at or below the changed index, after
  probing the new package) would keep the rest.
- Stale-answer and transient-open-failure risks listed in the feature's *Limitations* (inferred, not observed).

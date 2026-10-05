# Remember Missing Files: validation

The feature is described in [features/performance/remember-missing-files.md](../features/performance/remember-missing-files.md).
It must:

- Return 0 (and leave `*priorityOut` untouched) only when the game's own lookup would return 0.
- Skip probes of a counted database only while no write to it has happened since the entry's sum was taken.
- Keep a class probed when any of its studied functions differs.

## Automated tests

None offline. The lookup cache's developer checks cover these answers too.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No recorded in-game check results | |

## In-game test plan

Developer mode, with *Faster game file lookups* on:

1. **Expected log:** `[ResourceCache] Write epochs: DPF counted; DPF (derived) counted; DDF counted; packed stream
   counted` (any "probed (...)" names the class and why) and `[EntryChain] DPF direct record write (0x004a7fc0): layer 2
   installed`.
2. Developer line "Missing files (on)". **Expected:** "answered from memory" reaches about a third of all lookups after
   warm-up; "not remembered (a read-only package could not answer for sure)" small. "Package list: ... (asked on every
   answer unless counted: N)" gives the counted databases.
3. **Expected:** "Write epochs: ... answers with no probe of them" dominates the answers; "sums refreshed" grows slowly
   (about one wave per compositor / cache write); the average "packages asked" per answer falls towards 0 to 2.
4. *Check every answer for 10 s* in CAS (outfit changes write the CAS caches), in Build / Buy (lot saves write the lot
   DPF) and right after saving the game. **Expected:** still 0 different. A mismatch line says "(write epochs)" when the
   entry relied on them.
5. Frame Profiler: **Expected:** "Resource lookup" per hitch drops again (estimate: 4.1 ms to about 1.5 ms per hitch
   frame); hitch lines show ", absent from cache N".

## Confirmed in game

- Nothing recorded beyond the default-on release in 2.5.5.

## Open checks

- MemoryDB (needs an entry hook on PutResource 0x0072C350) and the ContentManager database (non-virtual install code)
  stay probed.
- A resolve-level cache at 0x007D8110 (key -> resolved key) would also remove the first failing lookup; not done.
- Write paths missed by the study (inferred risk, caught by the developer checks).

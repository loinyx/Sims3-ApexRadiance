# Faster Text Lookups: validation

The feature is described in [features/performance/faster-text-lookups.md](../features/performance/faster-text-lookups.md).
It must return the game's text for every key (the first table in list order with a non-empty text), answer "missing"
only when no table has a non-empty text, never answer from an index built before the list changed, and turn itself off
on the first difference from the game's walk.

## Automated tests

`tools/string_table_index_test/run.ps1 [-Exe <TS3W.exe>]` (offline, fake tables; the executable is only read):

- First non-empty text wins, an empty text is skipped, a missing key reaches the list's end, the first node and the
  sentinel come back when there is no answer.
- A hooked list change makes the index stale; the next lookups rebuild it in steps and the old answers stay.
- A list change no hook saw (first or last node moved) is not trusted; a text emptied behind the index's back is caught
  by the verification and turns the index off.
- Over the memory cap the index is not built; a changed table layout turns it off before any build.
- 3,000 tables (79,291 strings) against the walk, and the five signatures counted in the executable.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-20 | this branch | `run.ps1 -Exe` on Steam 1.67.2 and retail 1.67.2 `TS3W.exe` | All checks pass; each of the five signatures matches once in both executables; 3,000 tables: 1,536 KB, built in 23-36 ms over 12-18 lookups | None |
| 2026-11-20 | this branch | In game, four loads OFF, ON, ON, OFF (same save, 4,257 loaded string tables, most of them from custom content, Sims3Performance string cache and bucket layout tuning off); each load checks 917 UI, trait, skill and catalog names twice (before and after a CAS and Buy mode browse) and times 7,170 lookups (5 rounds over those keys plus as many missing keys) | Same text hash, 917 names, 14 empty, 0 placeholders in every check of all four loads. ON: `Index built (#1): 115026 strings from 4257 tables, 3072 KB, 33-38 ms in 14-16 steps`, no `Index off` line. Per lookup: OFF 68-79 us (missing keys 131-145 us), ON 2.0-2.5 us (missing keys 1.4-2.2 us) | RTX Remix bridge |

## In-game test plan

1. Turn on *Faster text lookups* and start the game. **Expected log:** `[StringTableIndex] On at 0x0059c5c2 ...`, then
   at the first text lookups `[StringTableIndex] Index built (#1): N strings from T tables, ...`.
2. Play: pie menus, CAS, Buy mode, tooltips. **Expected:** the same names as with the switch off; no
   `[StringTableIndex] Index off for this session` line.
3. Install or remove a package with string tables while the game runs (or travel). **Expected:** a new `Index built`
   line; names as before.
4. With Sims3Performance's *LocalizedStringCache* or *StblBucketLayoutTuning* on: **Expected:** `[StringTableIndex] Off:`
   with the reason; the game unchanged.

## Confirmed in game

- Same texts ON and OFF with 4,257 loaded string tables (see Latest results).

## Open checks

- EA app 1.69 signatures.
- Pie menu open time with the switch on and off on a quiet machine.
- A locale change (the game picks its language at start; the rebuild path is covered by the offline test).

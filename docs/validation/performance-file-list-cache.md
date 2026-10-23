# Faster File Lists: validation

The feature is described in [features/performance/file-list-cache.md](../features/performance/file-list-cache.md). It
must return the same keys from the same packages in the same package order as the game, for every cached request.

## Automated tests

None offline. The developer check runs the real calls for every package with a kept list (read-only, and writable of a
counted class) on 1 call in N and compares the keys as a set (plus the return value).

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-22 | `000f3bc` | None: no offline harness covers the writable packages' lists | Not run | not run in game yet |

## In-game test plan

1. **Expected log:** `[FileListCache] On: GetKeyList 0x004b1ae0 / 0x00736660 answered for the key type filter 0x00fd8248
   ...`.
2. Enter CAS, change outfits and hair of several Sims, load a household. **Expected:** "package lists from memory"
   dwarfs "packages asked" after the first pass; "File list checks: N equal, 0 different".
3. Frame Profiler. **Expected:** the "Key list" counter's ms per call drops sharply after the first call of each type;
   the CAS SimService hitches (0x005DA175 on the stack in sampling runs) shrink.
4. **Expected:** no file list mismatch line in the log.

## Confirmed in game

- Nothing recorded beyond the default-on release in 2.5.5.

## Open checks

- Memory use of kept lists in long sessions (the Developer line shows lists and keys kept).
- Shared key lists (`f58a6f0`): the Frame Profiler's "Key list" time on the render thread in CAS against the previous
  build (a lookup copied about 207 lists, 2 ms on average, up to 4 ms), and still no file list check difference.
- Writable packages' file lists (`000f3bc`), with *Faster game file lookups* and *Remember missing files* on: in CAS, in
  live mode and right after saving the game, with the Tester switch *Writable packages' file lists* off and on.
  **Expected:** the report's *writable packages of a counted class from memory* grows with it on;
  "File list checks: N equal, 0 different"; the "Key list" time on the render thread lower than with it off (the 22
  writable packages' walks were 1.2% of the render thread in a CPU sample). Not yet run in game.

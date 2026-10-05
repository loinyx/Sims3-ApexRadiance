# Faster File Lists: validation

The feature is described in [features/performance/file-list-cache.md](../features/performance/file-list-cache.md). It
must return the same keys from the same packages in the same package order as the game, for every cached request.

## Automated tests

None offline. The developer check runs the real calls for every read-only package with a kept list on 1 call in N and
compares the keys as a set (plus the return value).

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No recorded in-game check results | |

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

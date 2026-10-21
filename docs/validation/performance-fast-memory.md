# Faster Memory Handling: validation

The feature is described in [features/performance/fast-memory.md](../features/performance/fast-memory.md). It must:

- Change only the allocator lock's spin count and the timing of big-block releases.
- Never make an allocation fail because a release is still queued.
- Restore every call site and the spin count on Stop.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | | No recorded measurement | |

## In-game test plan

1. Turn it on. **Expected:** the Frame Profiler report line "Faster memory handling: ..." shows the spin 10 -> 2000 and
   releases done by the helper.
2. Sampling run before and after. **Expected:** fewer samples in the "system code called from" rows 004E5935 / 004E5954 /
   004E69E5 / 004E6A0B (allocator lock) and 004E530C (the release).
3. **Expected:** `VirtualAlloc` retries stay rare.

## Confirmed in game

- Nothing recorded beyond its release.

## Open checks

- The measured effect on hitches.
- 2.14.0: the allocator trim and top-down large blocks in game: the `[AllocatorTuning]` log lines, the megabytes
  decommitted per trim, the largest free block over a long session, and whether a crash recurs in the game's audio
  decoder (if it does, test with both off in a development build).

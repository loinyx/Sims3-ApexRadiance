# Faster Cache Compression: validation

The feature is described in
[features/performance/fast-cache-compression.md](../features/performance/fast-cache-compression.md). It must:

- Write streams that every RefPack decoder, the game's included, decompresses to the source bytes.
- Give a counting run and its write the same compressor, flags and depth, so the write has exactly the counted size.
- Never write past a non-zero capacity.
- Compute the record CRC-32 with the game's exact values.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/refpack_test/refpack_test.cpp`](../../tools/refpack_test/refpack_test.cpp) | Round trips through the game's decoder translation, stream checks, segmented streams, size and speed against the game's compressor translation | See below | x86 MSVC; console only, no files written |
| CRC scratch test (not in the repository) | Slicing-by-8 against the byte-wise loop with the table read from TS3W.exe | | Game executable |

Build and run from `tools\refpack_test` (same command prompt as `dxt_test`):

```
cl /nologo /O2 /EHsc /std:c++20 /I..\..\features refpack_test.cpp ..\..\features\refpack_codec.cpp /Fe:refpack_test.exe
refpack_test.exe        (--quick; --file path for real data)
```

Expected: "RESULT: every round trip and stream check passed". The timing lines and the size / speed table against the
game's compressor go into the report.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-30 | | `refpack_test`, 5.6 MB buffers, flags 2 | Segmented stream size within ±0.13% of `Compress`; one thread 10 to 20% slower than `Compress` (the window fill per piece); 7 threads 3.8x faster than one (thread start included), same bytes; about 3,000 segmented streams round-tripped through the game's decoder translation, piece-boundary sizes included | CPU |
| 2026-09-30 | build 0d1408ad | In game, Frame Profiler, the same cache writes before and after, hitch frames only | About 21 MB writes (4 calls: 2 counting runs and their writes) 299 ms (49 cases) -> 101 ms (3); about 11 MB 121 -> 43 ms; about 5.5 MB 78 -> 24 ms (44 / 6 cases); worst compression-dominated hitch 295 to 395 ms -> 135 ms. In game the pieces run about 2x faster than one thread (offline 3.8x): the game's own threads are busy at those moments | DXVK, maintainer's PC |
| 2026-09-30 | | CRC scratch test | 4,850 cases, all equal to the byte-wise loop | CPU |

## In-game test plan

Developer mode, Developer > Performance > *Compressed game data*:

1. Turn it on. **Expected log:** `[SlotChain] RefPack stream write: layer 3 installed ...` and `[FastRefPack] On: ...
   the game's decoder 0x004eb3b0`.
2. Play as for texture compression, save the game (the package writer's counting runs) and load it again.
   **Expected:** "Checks: N equal, 0 different", "writes after our counting run" > 0 after a save, "did not fit" 0.
3. Set *Also run the game's compressor on 1 stream in N* to 4 for a few minutes. **Expected:** a "size +x%, y.yx
   faster" line (this slows those calls; set it back to 0).
4. Frame Profiler: **Expected:** "RefPack compress" per-hitch ms drops. Load the saved game after turning the feature
   off: it loads normally.

## Confirmed in game

- 2026-09-30, build 0d1408ad: the kept counting-run stream and the segmented compressor reduced compression hitches as
  in the table. Maintainer feedback: the game felt smoother.

## Open checks

- Search depth (32) and nice length (96): pick them from `refpack_test`'s sweep and the developer comparison.
- Expected 5 to 20x on repetitive data with a few percent larger streams (inferred, from the bounded search).

# Faster Texture Compression: validation

The feature is described in
[features/performance/fast-texture-compression.md](../features/performance/fast-texture-compression.md). It must:

- Produce byte-identical DXT1 / DXT5 output to the game's encoders for every image size, including edge blocks and the
  edge-alpha quirk.
- Produce the same bytes serially and on several cores, under every x87 precision and MXCSR mode the caller uses.
- Turn itself off for the session on the first in-game difference.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/dxt_test/dxt_test.cpp`](../../tools/dxt_test/dxt_test.cpp) | `Fast` against `Ref` (the literal translation) on random and edge blocks; serial against parallel over the whole destination buffer; timing | See below | x86 MSVC; console only, no files written |

Build and run (x86 Native Tools Command Prompt for VS 2022, or after
`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"`), from `tools\dxt_test`:

```
cl /nologo /O2 /EHsc /std:c++20 /arch:SSE2 /fp:precise /I..\..\features dxt_test.cpp ..\..\features\dxt_codec.cpp /Fe:dxt_test.exe
dxt_test.exe        (10 million blocks per format; --blocks N, --seed N, --bmp file.bmp; --parallel-only)
```

Expected: "RESULT: all blocks identical"; the several-cores lines "0 different from serial, 0 from the reference;
counter differences 0, FP state mismatches 0", with some encodes "on one core because the pool was busy" in the
4-thread stress.

The offline test compares the fast path with the translation, not with the game. The in-game checks (first 16 textures
of every session, developer mode 1 in N) compare with the real function.

The parallel test covers 23 sizes (1 x 1 to 2051 x 7, non-multiples of 4, 1-pixel strips, padded source and
destination rows), 1 / 2 / 3 / 6 workers, row-sized and default chunks, under x87 53-bit, x87 24-bit, MXCSR
round-to-zero + FTZ + DAZ and both together, plus the reference; a 3,000-image stress of tiny splits and 4 threads
encoding at once (with different FP states) cover the hand-off.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-29 | | `dxt_test`, edge sizes | Found the DXT5 edge-alpha quirk reading the driver's locals; modelled in `FetchAlphaPartial` | CPU |
| 2026-09-29 | | `dxt_test`, mutation check | With the worker's FP-state load removed, the MXCSR runs differ in about 1.2 M blocks and the read-back counter fires in the x87 24-bit run (random data almost never flips the x87 test, so the read-back is the guard for the precision) | CPU |
| 2026-09-29 | | `dxt_test`, several cores (6 workers + caller) | 256 x 256 0.43 -> 0.09 ms (DXT1); 1024 x 1024 7.3 -> 1.5 ms (DXT1), 8.6 -> 1.55 ms (DXT5); 2048 x 2048 29 -> 5 ms (DXT1), 34.5 -> 5.6 ms (DXT5): about 5 to 6x on top of the fast path | CPU, maintainer's PC |

## In-game test plan

Developer mode, Developer > Performance > *Texture compression and processor cores*:

1. Turn it on. **Expected log:** `[EntryChain] DXT1 encoder (0x006152f0): layer 1 installed ...` (twice) and
   `[FastDxt] On: ...`.
2. Load a save, enter CAS, change outfits, pan over lots at different distances, change the season / weather, take a
   screenshot or a video capture. **Expected:** "Checks: N equal, 0 different" growing.
3. *Check every texture for 30 s* in CAS and while panning. **Expected:** still 0 different. Note "checked textures:
   game X ms, Apex Y ms (Zx)".
4. Frame Profiler on (either order). **Expected:** its Hooks table says "outer layer of the entry chain ...; the fast
   encoder is inside"; the "DXT encode" per-hitch ms drops by about the measured factor.
5. Visual: identical by construction; any difference would be a mismatch line in the log.
6. Several cores: **Expected:** the `[FastDxt] On:` line says "images from 256x256 split over this thread and up to N
   workers (N created)"; "Split textures" grows in CAS / lot views / terrain, "% by workers" well above 0, "FP state
   mismatches 0", "about X ms saved". Repeat step 3 with it on. Compare the Frame Profiler's "DXT encode" per hitch with
   *Use several cores* off and on; try the worker slider at 2 / 4 / max and the size slider at 128 / 256 / 512 while
   panning.

## Confirmed in game

- Nothing recorded beyond the default-on release in 2.5.5.

## Open checks

- In-game speed-up (expected 3 to 5x on the encoder, 44 to 49 ms hitches down to about 10 to 15 ms; inferred).
- The several-cores defaults (256 x 256, min(logical processors − 2, 6) workers) from the in-game "ms saved" and the
  profiler.
- Same `rcpss` / `rsqrtss` bits on hybrid CPUs (P / E cores).
- An AVX2 path would need proof that the VEX forms of `rcpss` / `rsqrtss` give the same results as the legacy ones.

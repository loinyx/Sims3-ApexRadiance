# Faster Sim Building: validation

The feature is described in [features/performance/fast-cas-sort.md](../features/performance/fast-cas-sort.md). It must
produce every triangle count and every sorted index list exactly as the game's function does.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/cas_sort_test/cas_sort_test.cpp`](../../tools/cas_sort_test/cas_sort_test.cpp) | The fast sort against `CasTriSort::Ref` on meshes packed like the game, with odd values: w = 0 or negative, repeated vertices, degenerate triangles, index counts not a multiple of 3, vertex counts over 65,536 | `cl /nologo /O2 /EHsc /std:c++20 /I..\..\features cas_sort_test.cpp ..\..\features\cas_tri_sort.cpp /Fe:cas_sort_test.exe` from `tools\cas_sort_test` | x86 MSVC; read-only, console only |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-30 | `1720843` | `cas_sort_test` | 565 meshes, every count and sorted list equal to the reference. Time: 1030 x 1500 21 -> 1.5 ms, 2013 x 3000 88 -> 2.5 ms, 3363 x 5000 242 -> 5.2 ms (8 threads, thread start included) | CPU |

## In-game test plan

1. Enter CAS, change hair and outfits of several Sims. **Expected:** no `[FastCas] Result differs from the game's` line.
2. Frame Profiler sampling run in CAS. **Expected:** the CAS SimService hitches no longer show 0x005D38F8 on the stack in
   most samples.

## Confirmed in game

- Nothing recorded beyond its release.

## Open checks

- None.

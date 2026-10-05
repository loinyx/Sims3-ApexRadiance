# Shader census and false colour: validation

The feature is described in [features/dev-tools/census.md](../features/dev-tools/census.md). It must:

- Change nothing in normal mode and cost nothing while false colour and the census are off.
- Paint only unclaimed lamp-lit draws, and count every candidate draw of the window once.
- Name dumped shaders by content hash and never write more than 300 refused-shader files per session.

## Automated tests

The census and false colour run only in game. The offline coverage tests below are read-only C++ harnesses kept in the
maintainer's scratchpad, outside this repository. They include `shader_patches.cpp`, run the patchers over captured
shaders or over every shader of `Shaders_Win32.precomp`, and validate the output with `D3DDisassemble`.

Rule: never write many files from a test executable. Kaspersky flagged one as ransomware (also ROADMAP-NIGHT-REMAKE.md,
risks). Harnesses print to stdout and write at most a handful of `.txt`/`.bin` dumps, one example per shader shape
(as `test14.cpp` does).

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `scratchpad\snowcover\test4.cpp` | Captured draws whose VS is an outdoor-object VS but whose PS `PatchObjectLampPs` refuses | `cl` (pattern below) | Windows SDK `d3dcompiler.lib` |
| `test5.cpp` | Why `PatchObjectLampPs` refuses each `ShadersRecusados` PS (first failing step) | Same | Same |
| `test6.cpp` | Object patch over refused and known-good shaders, validated | Same | Same |
| `test7.cpp` | Foliage patchers after the m63 generalisation | Same | Same |
| `test8.cpp`, `test9.cpp`, `test10.cpp` | `IsFloorVs`, `IsSnowFloorVs` / `PatchSnowFloor` over all captured shaders | Same | Same |
| `test11.cpp` | Wall lamp table: all ExteriorWall PS of the precomp -> `wall_lamp_table.h` | Same | Same |
| `test12.cpp` (`censotest.exe`, output `censotest.txt`) | Census follow-up: for every `SEM` line it loads `Censo\VS_/PS_<hash>.bin` and prints how the classifiers see the VS (`classe=objeto/nenhuma`) and whether the object and foliage PS patchers accept the PS | Same | Same |
| `test13.cpp` | Floor table: ExteriorFloors PS accepted by `PatchBakedAtlasPs` -> `floor_atlas_table.h` | Same | Same |
| `test14.cpp` (`countertest.exe`) | Counters/Phong coverage over `counterfam\` | `build14.bat` | Same |
| `fencetest`, `leaftest`, `roadtest*.ps1`, `snowtest.ps1` | Fence and instanced, leaf shadow, the 4 road variants (m06/m10/m18/m24), snow patches | Per folder | Same |
| `counter_sh\REPORT.md` | A Perl port of the object patchers over the 2,139 disassemblies; reproduced the C++ counts and planned the v5.6 generalisation | Perl | None |

Build pattern (`scratchpad\snowcover\build14.bat`):

```
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++20 /EHsc /O1 /I %USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter test14.cpp ^
   %USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\shader_patches.cpp d3dcompiler.lib /Fe:countertest.exe
```

Some tests `#include` `shader_patches.cpp` directly instead. Each loads `.bin` bytecode as DWORDs, runs a
`ShaderPatches::Patch*` / `Is*Vs` function, and checks the patched output with `D3DDisassemble` (valid = disassembles).
Also check instruction slots (ps_3_0 512 minimum) and that the patch does not claim unintended families (tests 8-10 and
13 check that InteriorFloor, terrain and water stay at zero).

Shader sources:

- captured shaders: `LightProbe-*\{VS,PS}_<ptr>.bin` (deduplicate by content);
- `Censo\*.bin`, `ShadersRecusados\*.bin`;
- every game shader: `Game\Bin\Shaders_Win32.precomp` (read-only), split by `scratchpad\passo3\shdlist.pl` (walks
  `VSHD`/`PSHD` blobs, finds the version token, trims at `0x0000FFFF`), with technique names from `TECH`/`PASS` records
  (`passo3\ground\techof.pl`, `techps.pl`, name hashes in `shd\knm.tsv`); extracted families in
  `scratchpad\counterfam\` (2,139 Counters/Phong variants), `floorfam\` (867), `passo3\...`.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-25 | Combined build | `test5.cpp` | 14 refused PS, 4 fixable | Offline |
| 2026-09-25 | Combined build | `test7.cpp` | 77 VS / 99 PS: foliage VS 6 -> 8, leaf-shadow PS 2 -> 3, all valid | Offline |
| 2026-09-25 | Combined build | `test8.cpp` to `test10.cpp` | Only the intended pairs match | Offline |
| 2026-09-25 | Combined build | `test11.cpp` | 58 ExteriorWall variants (K = c2 in 26, c3 in 32); no InteriorWall/AOSI/Unlit match | Offline |
| 2026-09-25 | Combined build | `test13.cpp` | 261 of 571 ExteriorFloors accepted; 0 of 58 InteriorFloor | Offline |
| Not recorded | Combined build (object patch v5.6) | `test14.cpp` | VS 588/588, Counters PS 136/184, Phong PS 426/444, 0 invalid | Offline |

## In-game test plan

1. Developer mode, Night Lights on (bridge on), at night on a test lot.
2. Tick false colour and walk around. **Expected:** magenta marks lamp-lit surfaces no fix handles; take a light capture
   on them.
3. Click Census. **Expected:** status returns to `(ready)`; log `[LotLightBridge] Census written: N pairs, M without a
   fix`; `ApexRadiance_Censo.txt` and `Censo\` in the Apex Radiance folder.
4. Map the `SEM` PS hashes to techniques offline (`techof.pl` on `Censo\PS_*.bin`), then run the relevant harness.
5. Developer mode off, restart. **Expected:** no magenta, no census controls, no new `ShadersRecusados` files.

Acceptance for Night Lighting coverage: no outdoor object in magenta on a summer and a winter test lot.

## Confirmed in game

- The census and false colour found the pairs listed in [history](../history/dev-tools-census.md) and drove the
  object, floor and Counters/Phong generalisations.

## Open checks

- Whether false colour changes what the Light Probe records for the same draw (inferred, not tested).
- Whether the census counts the Light Probe's occlusion copies when both run in one frame (inferred, not tested).

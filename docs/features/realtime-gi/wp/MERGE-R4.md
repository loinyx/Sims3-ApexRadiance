# Merge round 4 (MERGE-R4)

Branch `gi/merge-r4`, off `feature/realtime-gi` at merge round 3 (a1820bb). `feature/realtime-gi` fast-forwarded to it
once every test below passed.

Marks: [V] verified by a run, [I] inferred.

## Merged, in order

| Branch | Commit | Conflicts and how they were resolved |
|---|---|---|
| `gi/build-patch-fix` | f9177a6 | none ([BUILD-PATCH](BUILD-PATCH.md): patches carry upstream CRLF, `build.cmd` stops on a failed patch) |
| `gi/xr-08` | dee2476 | `realtime_gi.{h,cpp}`: kept the round 3 side (XR-07's settings were already in it); `README.md`: XR-08's patch 0020 line beside round 3's 0021 and 0022 lines; `gi_server_test/run.ps1`: XR-08's usage comment. Patch 0020 merged without conflict |
| `gi/gb-05` | 4bfb7e6 | none |
| `gi/xr-10` (with `gi/mc-4`) | cf1f82a | `realtime_gi.cpp`: `GiPixelShadows::Start` gets `meshCasters`; `Stop` clears `pixelCasts` with `pixelShadows` or `meshTiles`, beside round 3's scheduler and lamp-index lines. Patch 0022: round 3's version kept (rebased onto 0021, CRLF; it already has XR-10's time-bound mesh fetch poll). `README.md`: round 3's 0021 and 0022 lines |
| `gi/s2-05` | 80cf7d8 | `realtime_gi.h`: counter `IndirectDraws` after the lamp-index counters, `IndirectFor` after `LampIndexMode`; `realtime_gi.cpp` `Stop`: both sides' releases (pre-reset callback, scatter shader, grid and map layers; scheduler, lamp index, mesh set); `lot_light_bridge.cpp`: `g_roomMapGiPs` added to round 3's list of patched caches; runner `selftest.ps1`: both blocks (lamp index lines, indirect counters) |
| `gi/s2-05-slots` | 2eae8ed | none |

No patch file needed regenerating: `PATCH_ONLY=1 build.cmd` on a fresh exact checkout applied all 21 patches
(0001-0014, 0016-0022) in order [V].

## Extra fix
- `tools/gbuffer_check/run.ps1` links the check `/LARGEADDRESSAWARE`. GB-05's open item: the full DXVK x32 `movers`
  section (7,671 replay shaders) stalled in the 32-bit check process. With 4 GB of address space it completes [V].

## Tests (all from the `gi-merge-r4` worktree)

| Test | Result |
|---|---|
| Fresh `native/remix-bridge/build.cmd` (src deleted first) | built, exit 0 [V] |
| `test-gi.cmd`, `DXVK_X64` = the `native/dxvk` build: modes on, compute, off, stock, lots, lots-full, dxvk, dxvk-compute | GI server tests passed [V] |
| `test-gi-pixel.cmd` off and on | passed, `passes=202 rays=52437 faults=0` [V] |
| `tools/gi_server_test/run.ps1 -Mesh -Game <game folder> -Bridge` (with `test-gi-mesh.cmd`) | all checks passed [V] |
| `tools/gi_compose_test/run.ps1 -BridgeSrc native/remix-bridge/src` (S2-01b) | passed; scoped lamps byte-equal to unscoped [V] |
| `tools/gi_indirect_test/run.ps1 -Precomp <Shaders_Win32.precomp>` (S2-05) | 51 checks, 0 failures [V] |
| `tools/gbuffer_check/run.ps1 -Precomp ...` native D3D9 | 10,078 checks, 0 failures [V] |
| `tools/gbuffer_check/run.ps1 -Precomp ... -D3d9 <upstream DXVK x86 d3d9.dll>`, all sections incl. full `movers` | 20,152 checks, 0 failures, about 11 min [V] |
| `tools/gbuffer_motion_test/run.ps1` | 91 passed, 0 failed [V] |
| `tools/ingame_runner/selftest.ps1` | 660 checks, 0 failures [V] |
| `tools/rt_lamps_test/run.ps1` (XR-10) | 680,480 checks, 0 failures [V] |
| `tools/gi_server_test/run.ps1 -Pixel -TestArgs --precomp <Shaders_Win32.precomp>` (XR-08, XR-10) | all checks passed; precomp chain in 1238 of 1358 skinned shaders [V] |
| `tools/development/build.ps1 -Flavor Developer` and `-Flavor Public` | both succeed (1 warning each, as in round 3) [V] |
| `perl tools/menu_audit.pl .` | same counts as round 3 [V]; no `i18n/` or `ui/` change this round |

## Open
- In-game smokes of the merged packages are not run (the game belongs to the test chain): `gi-pixel-smoke`
  (variants `sims`, `meshes`, `on`), `gbuffer-motion-smoke` (`movers`, `taa`), `gi-indirect-smoke`, `mesh-casters-smoke`.
- `gi/s2-06` is not in this round (still being built).
- Open items of the merged packages stay in their own pages: [XR-08](XR-08.md), [XR-10](XR-10.md) (rig hook on the skin
  fill), [GB-05](GB-05.md), [S2-05](S2-05.md).

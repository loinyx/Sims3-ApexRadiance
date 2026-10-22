# Workflow: build, test, install, diagnose, release

This page is the operating manual for working on Apex Radiance: where things are, how to build and test the ASI, how
to install it on the test machine, how to read a problem report and how a version is released. The project rules in
[section 3](#3-project-rules) apply to every change. Framework internals are in [architecture.md](architecture.md).
The earlier combined-build process (Apex inside a fork of Sims3SettingsSetter) is kept in
[history/workflow.md](history/workflow.md).

## Paths

| What | Path |
|---|---|
| This repository | GitHub `loinyx/Sims3-ApexRadiance` |
| Game binaries (`TS3W.exe`, `Sims3LauncherW.exe`, ASIs, an optional DXVK `d3d9.dll`) | `<Game>\Bin\` (for example `...\The Sims 3\Game\Bin\`) |
| Game shaders (read-only reference) | `<Game>\Bin\Shaders_Win32.precomp` |
| Apex Radiance config, log and developer outputs | `%USERPROFILE%\Documents\Electronic Arts\The Sims 3\Apex Radiance\` |
| Official S3SS config and log | `...\The Sims 3\S3SS\` |
| Previous standalone (`S3SSApex.asi`), read once for migration | `...\The Sims 3\S3SS\Apex\` |
| MSBuild | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe` |

**Reference setup:** The Sims 3 Steam 1.67.2 with Ultimate ASI Loader as `wininet.dll`, tested both on native
Direct3D 9 and on DXVK.

---

## 1. Build

**Toolchain:** Visual Studio 2022 Build Tools, toolset v143, C++20, Release|Win32, static CRT (`MultiThreaded`),
whole-program optimisation. Dependencies (Dear ImGui with the dx9 and win32 bindings, Microsoft Detours, toml++) come
from vcpkg, triplet `x86-windows-static`, already installed in `vcpkg_installed\`. Always pass
`/p:VcpkgEnableManifest=false` so MSBuild does not try to reinstall them. Libraries: `imgui.lib`, `detours.lib`,
`d3d9.lib`, `d3dcompiler.lib`, `dxguid.lib`, `psapi.lib` and `shell32.lib` (project), plus `ole32.lib`, `version.lib`
and `windowscodecs.lib` through `#pragma comment`.

**Command** (one binary, `Release\ApexRadiance.asi`):

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" ApexRadiance.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false
```

In PowerShell use the call operator: `& "C:\Program Files (x86)\...\MSBuild.exe" ...`.

- **Public release asset:** build with `/p:ApexFlavorDefines=APEX_NO_DEV_TOOLS`, a separate `Public` output and
  intermediate directory. Publish that `ApexRadiance.asi`; it leaves out developer tools and cannot enable developer
  mode (`build_flavor.h`). The normal `Release` build retains the maintainer's developer tools and stays local.
- **`ApexPublic` is a legacy no-op.** The active public-build switch is `ApexFlavorDefines=APEX_NO_DEV_TOOLS`.
- **Files on disk that are not in `ApexRadiance.vcxproj` are not built.** A new `.cpp` must be added to the project's
  `ClCompile` list.
- `tools/development/build.ps1` runs the build through `tools/development/build_runner.mjs`, which checks the expected
  artifact and keeps the recipe and full log under the ignored `outputs/build/`.

### Development switches (`[dev_build]`)

A development build (`/p:ApexStudyDefines=APEX_DEVELOPMENT_BUILD_FLAVOR`, which defines `kCompiled` in
[`features/dev_switches.h`](../features/dev_switches.h)) reads the `[dev_build]` table of `ApexRadiance.toml` once at
start-up (`DevSwitches::Load`) and logs every value. A missing key reads as on; Save keeps the table because it
preserves tables it does not write. Each switch can be turned off to compare against the previous behaviour.

In every other build the accessors return `kShipped` (true) for the 28 switches that change the game or free memory, and false for the
3 diagnostics and the unfinished `foliage_sun_shadow`. A switch therefore never needs a menu option to ship; a shipped behaviour
follows the setting named on its page.

| Key | Public build | Effect | Page |
|---|---|---|---|
| `measurements` | Developer only | Address-space owners, game allocation categories and the memory CSVs | [memory-analysis.md](features/memory-analysis.md#development-flavour-measurements) |
| `daylight_log` | Developer only | Logs the sky state that sets outdoor darkness every 5 s and adds an F8 section; no lighting change | [light-diag.md](features/dev-tools/light-diag.md#file-sections) |
| `memory_owners` | Developer only | Apex's own memory by owner (`[ApexMemory]`, F8) | [memory-analysis.md](features/memory-analysis.md#development-flavour-measurements) |
| `release_debug_buffers` | Shipped | Gives back F7, F6 and F8 buffers once they are no longer needed | [memory-analysis.md](features/memory-analysis.md#development-flavour-measurements) |
| `left_lot_release` | Shipped | Gives back Quick outdoor lamp edits' kept parts of rooms no loaded lot holds (two 10 s sweeps in a row), Real-time outdoor lamps' CPU copy of the occluders once uploaded, and the mask texture of a lot not drawn for 2 minutes (made again at its next draw) | [performance/README.md](features/performance/README.md#the-switches) |
| `draw_reads_nowait` | Shipped | Buffer reads in draw hooks never wait for the GPU (three busy answers, then wait) | [lot-light-pass.md](features/night-lighting/lot-light-pass.md#lamp-shade-on-lot-ground-groundshadow) |
| `occlusion_relight` | Shipped | An unchanged mesh occlusion scene is not published again | [level-light-share.md](features/night-lighting/level-light-share.md#closed-floor-guards-directional-weights-and-the-retired-mesh-blockers) |
| `room_drain_budget` | Shipped | Extra room solves never start a step the budget cannot hold | [room-light-queue.md](features/performance/room-light-queue.md#automatic-lamps-first-and-the-step-budget) |
| `allocator_trim` | Shipped | The game allocator's own trim every 60 s and before saves | [fast-memory.md](features/performance/fast-memory.md#allocator-trim-and-top-down-large-blocks) |
| `allocator_top_down` | Shipped | The game allocator's large blocks from the top of the address space | [fast-memory.md](features/performance/fast-memory.md#allocator-trim-and-top-down-large-blocks) |
| `foliage_vertex_lamps` | Shipped | Lamp light on instanced foliage per vertex | [foliage.md](features/night-lighting/foliage.md#per-vertex-lamps-and-crown-shade) |
| `canopy_ground_shadow` | Shipped | Tree and bush crowns as porous occluders on lot ground | [foliage.md](features/night-lighting/foliage.md#per-vertex-lamps-and-crown-shade) |
| `foliage_sun_shadow` | Developer only | The sun and moon shadow on the tree shaders that read none | [foliage.md](features/night-lighting/foliage.md#the-suns-shadow-on-trees) |
| `auto_light_priority` | Shipped | A lamp switched on automatically puts its own room first | [room-light-queue.md](features/performance/room-light-queue.md#automatic-lamps-first-and-the-step-budget) |
| `lamp_visibility_memo` | Shipped | A lamp's wall and slab test is kept while its inputs are unchanged | [level-light-share.md](features/night-lighting/level-light-share.md#cost) |
| `indoor_lamp_walls` | Shipped | A lamp taken through a floor opening is also stopped by the walls of its own story and of the receiving story | [stairwells.md](features/night-lighting/stairwells.md), [level-light-share.md](features/night-lighting/level-light-share.md) |
| `arch_ambient` | Shipped | A floor near an arch takes the neighbour's higher ambient, fading over 1.5 m | [arches.md](features/night-lighting/arches.md) |
| `basis_wall_filter` | Shipped | Directional maps read without blending across a wall | [stairs.md](features/night-lighting/stairs.md) |
| `hard_wall_tops` | Shipped | A full wall under a ceiling has a hard top in the grid walk | [arches.md](features/night-lighting/arches.md) |
| `stair_height_light` | Shipped | A lamp of the story above lights the stairwell tiles from 1.5 m higher | [stairwells.md](features/night-lighting/stairwells.md) |
| `steady_indoor_light` | Shipped | Indoor light no longer tests the screen-collected meshes; one resend per lamp edit | [arches.md](features/night-lighting/arches.md) |
| `inner_norm_settle` | Shipped | Same-story imports use the receiving room's settled normalisation | [arches.md](features/night-lighting/arches.md) |
| `soft_opening_edges` | Shipped | A lamp cone through an arch or door spreads its edge over 0.3 m | [arches.md](features/night-lighting/arches.md) |
| `stair_room_fill` | Shipped | Stairs take their tile's room ambient as fill | [stairs.md](features/night-lighting/stairs.md) |
| `opening_bounce` | Shipped | A share of the other room's ambient through a stairwell opening | [stairwells.md](features/night-lighting/stairwells.md) |
| `stair_floor_tone` | Shipped | Stairs take the floor's tone curve | [stairs.md](features/night-lighting/stairs.md) |
| `stair_story_blend` | Shipped | Stairs blend with the story above by height | [stairs.md](features/night-lighting/stairs.md) |
| `no_opening_band` | Shipped | No band of the lower room's ambient beside a stairwell opening | [stairwells.md](features/night-lighting/stairwells.md) |
| `stairwell_bounce` | Shipped | Second bounce on the walls above and ceiling bounce onto the floor above | [stairwells.md](features/night-lighting/stairwells.md) |
| `borrowed_light_adapt` | Shipped | Partial adaptation (square root, at most x2) of rooms lit only from another story | [stairwells.md](features/night-lighting/stairwells.md) |
| `soft_slab_edges` | Shipped | Penumbra of a 0.25 m lamp disc at a floor opening's edge | [stairwells.md](features/night-lighting/stairwells.md) |
| `room_fill` | Shipped | Rooms lit only from another story take back their own reflected light | [stairwells.md](features/night-lighting/stairwells.md) |


---

## 2. Install

Install only with the game closed.

1. **Close the game and the launcher.** Neither `TS3W.exe` nor `Sims3LauncherW.exe` may be running (the launcher also
   holds `d3d9.dll` and files in `Bin`). Check with `tasklist | findstr /i "TS3W Sims3Launcher"`. Never copy into `Bin`
   while either is open, and never end the game process without asking.
2. **Back up first.** Copy the installed `Bin\ApexRadiance.asi` (the first time, the old `Bin\S3SSApex.asi`) and the
   configuration into a backup folder outside the game.
3. Copy `Release\ApexRadiance.asi` into `<Game>\Bin\`.
4. **Exactly one copy of the mod in `Bin`.** Delete the old `S3SSApex.asi` (previous standalone and combined-build
   name). The official `Sims3SettingsSetter.asi` stays next to `ApexRadiance.asi`. A leftover `S3SSApex.asi` is
   detected: if it loaded first, Apex Radiance stays idle and only writes an error to its log; otherwise the old one
   stays idle and Apex Radiance shows a banner. The old combined build makes Apex Radiance keep its features off
   ([architecture.md, section 11](architecture.md#11-coexisting-with-official-s3ss)).
5. Settings are not in the `.asi`. They live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance.toml`.
   On the first start without it, Apex Radiance copies `...\S3SS\Apex\Apex.toml` (previous standalone) as it is, else
   migrates the Apex tables from `...\S3SS\S3SS.toml` (combined build), backing it up as `S3SS.toml.pre-split.bak`.
   Developer mode applies only from the next game start.
6. Write down what to test and what to look for, so the result can be recorded in the validation page.

---

## 3. Project rules

The rules for every change are in [AGENTS.md](../AGENTS.md) (evidence before patches, English source text and
translations, menu review, credits, what is never committed) and [PUBLIC-TEXT-POLICY.md](PUBLIC-TEXT-POLICY.md).
In addition:

- **Name:** visible text says "Apex Radiance" (full: "Apex Radiance for The Sims 3") through `APEX_PRODUCT_NAME` and
  `APEX_PRODUCT_TAGLINE` in `apex_version.h`; the file is `ApexRadiance.asi`. Internal identifiers keep "Apex"
  (namespaces, `ApexPatch`, `APEX_` macros, `apex_*` source files).
- **Scope:** HDR output, Native HDR, Smooth Streaming, Script GC Scheduler and Service Frame Budget stay removed
  ([removed-features.md](removed-features.md)). Ambient Occlusion is a standalone GTAO feature
  ([features/ambient-occlusion.md](features/ambient-occlusion.md)).
- **License:** the project is MIT licensed ([LICENSE](../LICENSE)). The framework was written from scratch after the
  design of sims3fiend's Sims3SettingsSetter, credited in the README and Settings > Credits; no upstream files are
  included.

---

## 4. Testing

Most Apex Radiance code patches a game that cannot run under a test framework. Offline harnesses in `tools/` cover what
can be checked without the game: shader patchers over the real shader package, production policies extracted from
the source, GPU readback of Apex's own HLSL, and codec equivalence against translations of the game's functions. Each
feature's validation page (`docs/validation/<feature>.md`) lists its harnesses, how to run them and the latest results.

### Conventions

| Convention | Rule |
|---|---|
| Location | One folder per harness, `tools/<name>_test/` or `tools/<name>_check/`, with a `README.md` when it needs arguments or fixtures |
| Read-only | A harness never writes into the game folder, the capture folders or the repository. It prints to stdout. Compiler outputs and any scratch file go to an `-OutDir` / scratch directory (default under `%TEMP%`). Never write many files from a test executable: security software may flag it |
| Game closed | Harnesses do not attach to `TS3W.exe` and never install anything into the game. Run them with the game closed so the GPU and the shader package are not in use. The [in-game runner](#in-game-runner) is the exception, with its own safety rules |
| Runner | `run.ps1`, or `run_<topic>.ps1` for a sub-suite (for example `tools/terrain_lighting_test/run_queue_checks.ps1`). The runner compiles the harness with `cl.exe` and runs it, throwing on a compile error or a non-zero exit code |
| Compiler | MSVC x86 (`cl.exe`). Runners dot-source `tools/terrain_lighting_test/compiler_env.ps1` and call `Initialize-TerrainCompiler`, which finds Visual Studio with `vswhere`, runs `VsDevCmd.bat -arch=x86 -host_arch=x64` once and imports `PATH`, `INCLUDE`, `LIB` and `LIBPATH` (nested suites reuse an already initialised toolchain). Harnesses without a runner give their `cl` command in their README, to run from an x86 developer prompt |
| Production code | Harnesses compile the production source (`features/shader_patches.cpp`, policy headers) or extract the exact production function from it at run time. An extraction fails loudly when the source boundaries change, so the fixture never silently tests stale code |
| Baselines | Optional `-ReferenceSource`, `-Baseline` or `-SourcePath` arguments take a source snapshot of the parent commit, so old and new behaviour are compared on the same fixtures |
| GPU | GPU checks create a real Direct3D 9 HAL device on a small hidden window (16x16 or 64x64 back buffer) and read pixels back. By default this is native D3D9. To check the game's backend, place the game's DXVK `d3d9.dll` next to the test executable in the output folder (never the other way round); harnesses print the loaded `d3d9.dll` path and the adapter so the backend is recorded |
| Fixtures | Captured shaders (`PS_<hash>.bin`, `VS_<hash>.bin` from Light Probe captures) and `Shaders_Win32.precomp` are read from their original location and never copied into the repository |
| Result | Each executable prints `N checks, 0 failures` (or its own counters, such as `checks: N; failures: 0`) and returns a non-zero exit code on any failure. A missing fixture or an unavailable D3D9 device is a failure, not a skip |

### What a passing harness proves

A fixture proves that a code path behaves as specified on the inputs it constructs: a patcher accepts or refuses a
shader, a policy picks the right action, a shader renders the expected pixels, a codec is bit-identical to the game's.
It does **not** prove what the player sees in game, frame times, or behaviour on scenes, mods or drivers the fixture
does not model. Record a harness result in the validation page with its date, commit, harness, result and backend, and
keep in-game confirmation as a separate item.

### Suites

`tools/development/check.ps1 -Suite <name>` runs a group of harnesses that need no game files and throws on the first
failure; CI runs `Smoke` and `Lighting`.

| Suite | Runs |
|---|---|
| `Smoke` / `Full` | DXT and RefPack codecs, CAS triangle sort (`Full`: the long variants) |
| `Lighting` | `tools/stairwell_bounce_test/`, `tools/opening_bounce_test/`, `tools/arch_ambient_test/` (pure lighting helpers, no captures) |

### Harnesses

| Harness | Covers |
|---|---|
| `tools/ao_receiver_test/` | Sim receiver shader copies for Ambient Occlusion, composite rendering, extracted production replay order (`run_runtime_checks.ps1`) |
| `tools/arch_ambient_test/` | Room ambient through arches (`features/arch_ambient.h`, `run.ps1`) |
| `tools/basis_shadow_check/` | Stairs on the directional maps: wall mask, floor-map read, room fill, floor tone and story blend on the game's stair and object shaders (`run.ps1 -Precomp <Shaders_Win32.precomp> -Captures <folder>`, native D3D9) |
| `tools/canopy_test/` | Crowns as porous occluders (`features/foliage_canopy.h`, `run.ps1`) |
| `tools/basis_test/` | Smooth room light patches (`PatchBasisSmooth`, `PatchIndoorBasis`) on captured shaders, with disassembly |
| `tools/cas_sort_test/` | Fast CAS triangle sort against the translation of `0x005D1960` |
| `tools/developer_mode_check/` | Developer-mode persistence and profile handling (`prepare.py` builds the fixture) |
| `tools/foliage_vertex_test/` | Per-vertex foliage lamps on the captured foliage and trunk vertex shaders against a CPU reference (`run.ps1 -Shader <vs.bin> -Trunk <vs.bin>`, native D3D9) |
| `tools/game_alloc_categories_test/` | The development flavour's game allocation category tables under concurrency (`run.ps1`) |
| `tools/dxt_test/` | Fast DXT1 / DXT5 encoder against the translation of `0x006152F0` / `0x006154B0` |
| `tools/i18n_check/` | Translation tables: conflicts, missing languages, placeholder and `##id` mismatches; coverage and stale keys of the other languages; `i18n/keys.tsv` up to date (`tools/i18n_keys.pl` writes it) |
| `tools/liveness_prune_test/` | Pruning of per-story caches by live story managers (`features/liveness_prune.h`, `run.ps1`) |
| `tools/loading_gate_test/` | Loaded-world reader and the extracted start-notice and Depth Blur loading guards (`run.ps1`) |
| `tools/memory_owners_test/` | Apex memory by owner: `operator new` counts, container estimates, Direct3D resources on a real device (`run.ps1`, native D3D9 and DXVK) |
| `tools/mesh_occlusion_test/` | The mesh blockers' collector, BVH and replacement policy (`run.ps1 -OpaqueShader <ps.bin> -CutoutShader <ps.bin> -Standard` or `-Development`) |
| `tools/opening_bounce_test/` | Bounce light through floor openings (`features/opening_bounce.h`, `run.ps1`) |
| `tools/post_scene_test/` | Post-scene boundary on a native D3D9 device |
| `tools/refpack_test/` | Fast RefPack compressor against the game's decompressor and compressor |
| `tools/report_check/` | Report a problem: capture storage, recorder, overlay clock and loading gate, menu rendering in four languages |
| `tools/room_ambient_test/` | Room updater policy and Rooms at Night recovery (`run.ps1`) |
| `tools/room_structure_test/` | Structural room edits (`run.ps1`) |
| `tools/room_map_smooth_check/` | Smooth light maps: the room-lit shader pattern over the precomp and captures, the copies (disassembly, native creation under the device's caps), the paired vertex shaders' uv scale, GPU pixels against a CPU B-spline with the wall band clamp, the dither offset (`run.ps1 -Precomp <Shaders_Win32.precomp> [-Captures <folder>]`) |
| `tools/roofed_openings_test/` | Outdoor and indoor light through openings: story grid walk, roofed rooms' doors, openings into roofed rooms, foundation sides (`run.ps1`) |
| `tools/spatial_aa_check/` | Spatial SMAA compilation and rendering |
| `tools/stairwell_bounce_test/` | Second bounce through a stairwell opening, room fill, borrowed-light normalisation, slab edge penumbra (`features/stairwell_bounce.h`, `features/penumbra.h`, `run.ps1`) |
| `tools/temporal_check/` | Temporal jitter refusal and reprojection for Edge Smoothing |
| `tools/terrain_lighting_test/` | Terrain lighting policies, shader patches, sampler guard, queue, resources, lamp state, dispatch, world pool, indoor stories, lot UV, ground scale (`run.ps1` and `run_*.ps1`) |
| `tools/water_lamp_check/` | Water lamp shader equivalence and highlights |
| `tools/world_lamp_test/` | World lamp eligibility policy |

### In-game runner

[`tools/ingame_runner/`](../tools/ingame_runner/README.md) runs features in the real game, unattended: it installs a
built ASI and a test agent (a script mod), starts the game once per load with that load's `ApexRadiance.toml`, loads
a test save, drives the job's steps (speed, camera pan, saves, reloads), checks `ApexRadiance_LOG.txt` against the
job's expected and failure lines, records frame times when asked and restores every file it replaced. A feature is
compared as separate game starts in ABBA order. It is the one tool that starts the game; it refuses to run while the
game or the launcher is open. `tools/ingame_runner/selftest.ps1` checks it offline.

### Shader-patch changes

`Shaders_Win32.precomp` holds every game shader and is the coverage reference (for example the 58 ExteriorWall pixel
shaders in `wall_lamp_table.h`). Shader-patch changes are tested over every captured shader before a game test,
disassembling the result with `D3DDisassemble` ([features/dev-tools/census.md](features/dev-tools/census.md)).

---

## 5. Diagnosing a problem

Ask first exactly what was seen: where, time of day, season, which lot, a screenshot. Then:

1. **Log:** `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance_LOG.txt` (header "Apex Radiance Log";
   fallback `Game\Bin\ApexRadiance_LOG.txt`). It records the game build, the graphics card and driver, the mods and
   wrappers in `Game\Bin`, `Options.ini`, S3SS and old-build detection, the `[Config] Migration path:` line, every
   address and pattern found or missing (`[Addr]`), every hook attach and whether its target was already hooked
   (`[D3D]`), and shader patchers that found no expected pattern. Feature status lines are on each feature card and,
   in developer mode, on the Developer page.
2. **Crash:** `ApexRadiance_Crash.txt` and `ApexRadiance_Crash.dmp` in the same folder (exception, registers, modules,
   the features that were on).
3. **Report a problem** (menu page): saves the log, the settings, recordings and captures into a dated folder under
   `Captures\` ([features/bug-reports.md](features/bug-reports.md)).
4. **Light Probe** (developer mode): GPU capture of the draws covering the pixel under the cursor: shaders, constants,
   textures. Output `ApexRadiance_LightProbe.txt` and the `LightProbe\` folder; archived captures are named
   `LightProbe-<name>` / `LightProbe-mNN` in the notes. See [features/dev-tools/light-probe.md](features/dev-tools/light-probe.md).
5. **Light Diag** (developer mode): the light manager state, lamps, per-pixel lamp section and level-share decisions in
   `ApexRadiance_LightDiag.txt` (writer `features/light_diag.cpp`, not the untracked `patches/light_diag_patch.cpp`).
   See [features/dev-tools/light-diag.md](features/dev-tools/light-diag.md).
6. **Frame Capture** (developer mode): a draw-by-draw list of two consecutive frames (`kFramesPerCapture = 2`) in
   `ApexRadiance_FrameCapture.txt`. See [features/dev-tools/frame-capture.md](features/dev-tools/frame-capture.md).
7. **Census** (developer mode): `ApexRadiance_Censo.txt` and `Censo\`, which shader pairs are drawn and which have no
   correction, with a false-colour mode. See [features/dev-tools/census.md](features/dev-tools/census.md).
8. **Performance:** the Frame Profiler ([features/frame-profiler.md](features/frame-profiler.md)) with per-service and
   per-hook timing, sampling, and `ApexRadiance_Hitches.txt` for long frames.
9. Compare with the notes before theorising: most surfaces have a recorded capture and root cause
   ([features/night-lighting/README.md](features/night-lighting/README.md)).

Shortcuts for the developer tools follow the selected shortcut set (Settings > Shortcuts, `hotkeys.h`), always with
Ctrl+Shift: function keys F7 Light Probe, F8 Light Diag, F6 recording, F5 Frame Capture; letter keys V, B, X, F; number
row 4, 5, 6, 7. Older documents name Ctrl+Shift+F9 for Frame Capture, which is now Refresh in the function-key set. Older notes and history pages
quote the combined build's file names (`S3SS_LOG.txt`, `S3SS_LightProbe.txt`, `S3SS_Hitches.txt`, ...) in the S3SS
folder. The previous standalone wrote `Apex_*.txt` under `...\S3SS\Apex\` (left in place, not migrated).

---

## 6. Release

The review, documentation and publication steps are in [release-process.md](release-process.md).

Published versions are listed in [releases/README.md](releases/README.md).

---

## 7. Branches, pull requests and repository skills

Branch names, commit and pull request titles follow [CONTRIBUTING.md](../CONTRIBUTING.md#branches-commits-and-pull-requests)
and [PUBLIC-TEXT-POLICY.md](PUBLIC-TEXT-POLICY.md). Concurrent contributors use separate worktrees and branches; fetch
and inspect new remote commits before pushing and never force-push over someone else's work. Merging and releasing are
maintainer actions.

The [apex-menu](../.agents/skills/apex-menu/SKILL.md) skill (discovery copy in `.claude/skills/apex-menu`, kept
identical) covers menu changes. Its audit, `perl tools/menu_audit.pl .`, reports untranslated texts, copy-guideline
breaks, undocumented keys and settings missing from ResetDefaults; it is heuristic, so check runtime text and
persistence by hand.

## See also

- [architecture.md](architecture.md): framework internals.
- [releases/README.md](releases/README.md): published versions.
- [history/workflow.md](history/workflow.md): the combined-build build and release process.
- [DOCUMENTATION-STANDARD.md](DOCUMENTATION-STANDARD.md): how pages are written.

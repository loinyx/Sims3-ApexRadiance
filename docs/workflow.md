# Workflow: build, test, install, diagnose, release

This page is the operating manual for working on Apex Radiance: where things are, how to build and test the ASI, how
to install it on the test machine, how to read a problem report and how a version is released. The project rules in
[section 3](#3-project-rules) apply to every change. Framework internals are in [architecture.md](architecture.md).
The earlier combined-build process (Apex inside a fork of Sims3SettingsSetter) is kept in
[history/workflow.md](history/workflow.md).

## Paths

| What | Path |
|---|---|
| This repository (Apex Radiance) | `%USERPROFILE%\Documents\Projetos\ApexRadiance\` (GitHub `loinyx/Sims3-ApexRadiance`) |
| Combined build (frozen reference, read-only; tag `combined-final`, commit `45e36e2`, branch `night-remake`) | `%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\` |
| Game binaries (`TS3W.exe`, `Sims3LauncherW.exe`, ASIs, DXVK `d3d9.dll`) | `C:\Games\Hydra\The Sims 3\Game\Bin\` |
| Game shaders (read-only reference) | `C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp` |
| Apex Radiance config, log and developer outputs | `%USERPROFILE%\Documents\Electronic Arts\The Sims 3\Apex Radiance\` |
| Official S3SS config and log (also the combined build's) | `...\The Sims 3\S3SS\` |
| Previous standalone (`S3SSApex.asi`), read once for migration | `...\The Sims 3\S3SS\Apex\` |
| Backups | `%USERPROFILE%\Desktop\Backups Sims 3\<NN-description>\` |
| Knowledge notes (Portuguese, chronological; later entries win) | `%USERPROFILE%\Desktop\S3SS-dev\NOTAS-ILUMINACAO.md`, `PASSO3-PLANO.md`, `PLANO-SEPARACAO.md`, `ROADMAP-NIGHT-REMAKE.md` |
| Ghidra project and decompile output | `%USERPROFILE%\Desktop\S3SS-dev\re\` (`re\out` = decompiled C per function) |
| MSBuild | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe` |
| GitHub CLI | `C:\Program Files\GitHub CLI\gh.exe` (logged in as `loinyx`) |

**Test machine:** The Sims 3 Steam 1.67.2 at 4K on an RTX 4070 Ti SUPER. `d3d9.dll` is the official DXVK 3.1.1
(sha256 starts `265888c3`; it was once misread as "2.0"). Ultimate ASI Loader 9.7.4 is installed as `wininet.dll`,
next to `Sims3Performance.asi` and `MonoPatcher.asi`. Nothing in `Bin` other than the mod's `.asi` is changed without
an explicit request.

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

- **One unified binary.** The ASI contains the optional developer tools. Developer mode is off by default and is read
  from `[ui] developer_mode` at startup, before the features are created. It is turned on in Settings > Menu after a
  confirmation and applies after a game restart. See [features/developer-mode.md](features/developer-mode.md).
- **`ApexPublic` is a legacy no-op.** Never produce separate player and developer assets.
- **Files on disk that are not in `ApexRadiance.vcxproj` are not built.** A new `.cpp` must be added to the project's
  `ClCompile` list.
- The [apex-compile-project](../.agents/skills/apex-compile-project/SKILL.md) skill finds, runs and caches a verified
  build command, checks the expected artifact and keeps the full log local.

---

## 2. Install

Install only with the game closed.

1. **Close the game and the launcher.** Neither `TS3W.exe` nor `Sims3LauncherW.exe` may be running (the launcher also
   holds `d3d9.dll` and files in `Bin`). Check with `tasklist | findstr /i "TS3W Sims3Launcher"`. Never copy into `Bin`
   while either is open, and never end the game process without asking.
2. **Back up first** ([rule 1](#3-project-rules)). Copy the installed `Bin\ApexRadiance.asi` (the first time, the old
   `Bin\S3SSApex.asi`) into a new numbered folder `Backups Sims 3\<NN-description>\`, for example
   `ApexRadiance.asi.instalado-antes`, and any source file about to be rewritten (`<file>.antes-<change>`). The next
   folder number is recorded in `CLAUDE.md`.
3. Copy `Release\ApexRadiance.asi` into `C:\Games\Hydra\The Sims 3\Game\Bin\`.
4. **Exactly one copy of the mod in `Bin`.** Delete the old `S3SSApex.asi` (previous standalone and combined-build
   name). The official `Sims3SettingsSetter.asi` stays next to `ApexRadiance.asi`. A leftover `S3SSApex.asi` is
   detected: if it loaded first, Apex Radiance stays idle and only writes an error to its log; otherwise the old one
   stays idle and Apex Radiance shows a banner. The old combined build makes Apex Radiance keep its features off
   ([architecture.md, section 11](architecture.md#11-coexisting-with-official-s3ss)).
5. Settings are not in the `.asi`. They live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance.toml`.
   On the first start without it, Apex Radiance copies `...\S3SS\Apex\Apex.toml` (previous standalone) as it is, else
   migrates the Apex tables from `...\S3SS\S3SS.toml` (combined build), backing it up as `S3SS.toml.pre-split.bak`.
   Developer mode applies only from the next game start.
6. Tell the tester (in Portuguese) what to test and what to look for.

---

## 3. Project rules

1. **Back up before modifying** any game file, mod file, config, driver or source file about to be rewritten, into
   `%USERPROFILE%\Desktop\Backups Sims 3\<numbered folder>`. The scratchpad alone is never a backup.
2. **No guessing: study before implementing.** Static deduction failed repeatedly on this project. The method that
   works: (a) GPU probe capture (Light Probe), (b) Light Diag, (c) read the actual shader and its constants,
   (d) patch by pattern and test offline over all captured shaders, (e) adversarial review. Mark anything not verified
   as *Unverified*, in code comments and in docs.
3. **English is the source language** of UI text, tooltips and status lines. Logs, the Developer page and file names
   are English only. The menu is translated into Portuguese (Brazil), Spanish and French (Settings > Menu >
   Language; automatic = the Windows display language). New texts are written in English and their translations added
   to `i18n/tr_*.cpp` (how: `i18n/TRANSLATING.md`). The maintainer communicates in Portuguese; replies to the
   maintainer are in Portuguese.
4. **Credits:** "Credits: @loinyx" appears only at the end of each feature description shown on hover. Other credits
   live in Settings > Credits: sims3fiend (Sims3SettingsSetter, the model for the rewritten framework), FXAA and
   third-party code (Dear ImGui, Microsoft Detours, toml++, SMAA, Lucide icons under ISC), and one line for
   Every-Story Ground Light (`apex_gui.cpp`: "Upper-floor ground light: Arro's technique, adapted in Apex Radiance.").
   The README credits carry the same attribution with a link to arro-now.tumblr.com. Arro is never named in feature
   descriptions, tooltips, release notes or promotional text, and the wording never implies the whole mod is based on
   that work. Sims3SettingsSetter is named only in compatibility notices, detection, the S3SS recommendation card,
   Credits and the S3SS.toml migration.
5. **Menu organisation:** main controls visible, tuning in collapsed "Advanced" areas, developer tools apart (Developer
   page, shown only in developer mode). Every new control gets English text, a hint, a sane default and translations.
   After adding any feature or option, review the whole menu with the [apex-menu](../.agents/skills/apex-menu/SKILL.md)
   skill and run `perl .claude/skills/apex-menu/audit.pl .` from the repository root (details in
   [section 7](#7-branches-pull-requests-and-repository-skills)). Theme and layout: [ui.md](ui.md).
6. **Name:** visible text says "Apex Radiance" (full: "Apex Radiance for The Sims 3") through `APEX_PRODUCT_NAME` and
   `APEX_PRODUCT_TAGLINE` in `apex_version.h`; the file is `ApexRadiance.asi`. Never "S3SS Apex" or "Apex Edition"
   again. Internal identifiers keep "Apex" (namespaces, `ApexPatch`, `APEX_` macros, `apex_*` source files).
7. **Git:** commits use the maintainer's identity (loinyx). Never commit the game's shader bytecode (`*_ref.h`), the
   developer leftovers `patches/call_trace_patch.cpp`, `patches/lot_edge_lighting_patch.cpp`,
   `patches/light_diag_patch.cpp` and `trace_targets.h`, or `Public/`. Commit only on request.
8. **Publishing, pushing, releases and repository renames** happen only with the maintainer's explicit approval.
9. **Scope:** HDR output, Native HDR, Smooth Streaming, Script GC Scheduler and Service Frame Budget stay removed
   ([removed-features.md](removed-features.md)); they are not restored without a request. Ambient Occlusion is a
   standalone GTAO feature ([features/ambient-occlusion.md](features/ambient-occlusion.md)). Smooth Patch Precise
   improvements stay out of Apex Radiance (upstream pull-request candidate).
10. **Licensing:** upstream S3SS has no licence. The framework was rewritten from scratch, so no sims3fiend file headers
    are carried. Ask sims3fiend before publishing under a new licence arrangement.
11. **New work goes into this repository**, never into the frozen combined tree.

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
| Read-only | A harness never writes into the game folder, the capture folders or the repository. It prints to stdout. Compiler outputs and any scratch file go to an `-OutDir` / scratch directory (default under `%TEMP%`). Never write many files from a test executable: one was flagged as ransomware by Kaspersky |
| Game closed | Harnesses do not attach to `TS3W.exe` and never install anything into the game. Run them with the game closed so the GPU and the shader package are not in use |
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

### Harnesses

| Harness | Covers |
|---|---|
| `tools/ao_receiver_test/` | Sim receiver shader copies for Ambient Occlusion, composite rendering, extracted production replay order (`run_runtime_checks.ps1`) |
| `tools/basis_test/` | Smooth room light patches (`PatchBasisSmooth`, `PatchIndoorBasis`) on captured shaders, with disassembly |
| `tools/cas_sort_test/` | Fast CAS triangle sort against the translation of `0x005D1960` |
| `tools/developer_mode_check/` | Developer-mode persistence and profile handling (`prepare.py` builds the fixture) |
| `tools/dxt_test/` | Fast DXT1 / DXT5 encoder against the translation of `0x006152F0` / `0x006154B0` |
| `tools/i18n_check/` | Translation tables: conflicts, missing languages, placeholder mismatches |
| `tools/loading_gate_test/` | Loaded-world reader and the extracted start-notice and Depth Blur loading guards (`run.ps1`) |
| `tools/post_scene_test/` | Post-scene boundary on a native D3D9 device |
| `tools/refpack_test/` | Fast RefPack compressor against the game's decompressor and compressor |
| `tools/report_check/` | Report a problem: capture storage, recorder, overlay clock and loading gate, menu rendering in four languages |
| `tools/room_ambient_test/` | Room updater policy and Rooms at Night recovery (`run.ps1`) |
| `tools/room_structure_test/` | Structural room edits (`run.ps1`) |
| `tools/spatial_aa_check/` | Spatial SMAA compilation and rendering |
| `tools/temporal_check/` | Temporal jitter refusal and reprojection for Edge Smoothing |
| `tools/terrain_lighting_test/` | Terrain lighting policies, shader patches, sampler guard, queue, resources, lamp state, dispatch, world pool, indoor stories, lot UV, ground scale (`run.ps1` and `run_*.ps1`) |
| `tools/water_lamp_check/` | Water lamp shader equivalence and highlights |
| `tools/world_lamp_test/` | World lamp eligibility policy |

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

Every release follows [apex-review-pr-release](../.agents/skills/apex-review-pr-release/SKILL.md) before publication.

1. **Review.** Substantive code gets an independent review and an evidence-based discussion of bugs and regressions,
   performance cost, preservation of the intended look and documentation consistency. Demonstrated bugs are fixed.
   Record the exact reviewed and tested commit SHA and what gameplay or performance validation is still open
   (*Unverified*). Changes added after the review are reviewed before publishing.
2. **Build once** (section 1) and check the translations (`tools/i18n_check`, the apex-menu audit).
3. **Install and test** the build in game (section 2). Record results in the validation pages.
4. **Documentation:** update the feature, validation and history pages, and write the release page in
   [releases/](releases/README.md) using its common layout.
5. **Publish only after the maintainer's explicit approval.** The release asset is the unified `ApexRadiance.asi`
   built from the reviewed commit; verify after publication that the published asset matches the reviewed build.
   Repository: `loinyx/Sims3-ApexRadiance`.
6. **Release notes:** a short `##` headline, player-facing `- **Title:** description` bullets and `## Install`. The
   Nexus workflow derives its changelog from that body. Keep internal provenance notes out of Git and source archives.
   A documentation-only follow-up does not require rebuilding or replacing the published ASI.

Published versions are listed in [releases/README.md](releases/README.md). The current published version is 2.5.6;
pull request #2 is in development and has not been published.

---

## 7. Branches, pull requests and repository skills

New versions and features are developed on a branch with small coherent commits and a draft pull request. The same
pull request is resumed across sessions, with its scope, tested commit and remaining validation kept visible.
Concurrent contributors use separate worktrees and contribution branches, with one integrator updating the shared pull
request. Fetch and inspect new remote commits before pushing; never force-push over someone else's work. Merging and
releasing are explicit actions.

| Skill | Use |
|---|---|
| [apex-version-pr-workflow](../.agents/skills/apex-version-pr-workflow/SKILL.md) | Start or resume a version branch, commits and draft pull request |
| [apex-compile-project](../.agents/skills/apex-compile-project/SKILL.md) | Build with a verified, cached command; check the artifact; keep logs local |
| [apex-review-pr-release](../.agents/skills/apex-review-pr-release/SKILL.md) | Pre-release review: independent review, evidence-based debate, recorded SHA and open validation |
| [apex-menu](../.agents/skills/apex-menu/SKILL.md) | Plan or review menu changes; `audit.pl` reports untranslated texts, copy-guideline breaks, undocumented keys and settings missing from ResetDefaults |

The apex-menu skill has a Claude Code discovery copy in [.claude/skills/apex-menu](../.claude/skills/apex-menu/SKILL.md);
keep both copies and their audit scripts in sync. The audit uses Perl, takes the repository root as its argument and
reports heuristically: check runtime text and persistence by hand. Skill caches, checkpoints and task scratch or output
directories are ignored by Git; machine-specific build recipes and binaries are never committed.

## See also

- [architecture.md](architecture.md): framework internals.
- [releases/README.md](releases/README.md): published versions.
- [history/workflow.md](history/workflow.md): the combined-build build and release process, and earlier backup folders.
- [DOCUMENTATION-STANDARD.md](DOCUMENTATION-STANDARD.md): how pages are written.

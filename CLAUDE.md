> Release 2.12.0 adds 3D CUBE LUTs, portable preset packages and graphics-resource restart. Public assets omit developer tools; legacy TOML presets remain supported. See the feature and validation pages.

> Release 2.11.0 adds independent Color groups and optional filter shortcuts, removes half-resolution AO and recognizes both recorded hidden-UI colour-write states. See the current feature and validation pages; final-image parity across every backend is not established.

> Current release 2.10.1: exterior wall lighting reuses the existing per-point floor lookup. The reported Build/Buy scene updates faster; no numerical latency/FPS gain is claimed. Lighting parameters and solve policies are preserved. Release assets use `ApexFlavorDefines=APEX_NO_DEV_TOOLS`; developer builds remain local. Read the affected feature/validation/history pages before further lighting changes.

# CLAUDE.md: Apex Radiance

## What this is
**Apex Radiance** ("Apex Radiance for The Sims 3"; file `ApexRadiance.asi`; author @loinyx; renamed 2026-09-28 from
"Sims3 Settings Setter Apex Edition" / "S3SS Apex" / `S3SSApex.asi`) is a native mod for The Sims 3 (Steam 1.67.2,
`TS3W.exe`, 32-bit). It is an ASI loaded by Ultimate ASI Loader, compatible with an unmodified official
Sims3SettingsSetter but not dependent on it. It hooks the D3D9 device and patches game
code in memory: Detours, pattern scans, ImGui menu, TOML config. Visible names come from `apex_version.h`
(`APEX_PRODUCT_NAME`, `APEX_PRODUCT_TAGLINE`); internal identifiers keep "Apex" (namespaces, `ApexPatch`, `APEX_`
macros, `apex_*` source files).

Features: Night Lighting (rebuilt night lamp light on ground, roads, floors, walls, roofs, water, foliage, objects,
fences, snow; key `NightTerrainRelight`), Every-Story Ground Light (`SplitLevelGroundLight`, lot lamps on any story
light the ground; part of Night Lighting), Reflections, Picture filters (SDR), Edge Smoothing
(SMAA/FXAA), Depth Blur, Performance (12 individually adjustable switches, enabled by default when no saved choice
exists; none marked experimental; grouped behind one Performance switch in Overview and included in its All effects
switch; offline tests in `tools\dxt_test` and `tools\refpack_test`; `docs/features/performance/README.md`), Frame Profiler (developer mode only), plus dev tools (Light Probe
Ctrl+Shift+F7, Light Diag Ctrl+Shift+F8, Frame Capture Ctrl+Shift+F9, Lot Map Probe, census). Menu: Violet UI
(sidebar plus feature cards), hotkey Ctrl+Shift+F11. Smooth Streaming, Script GC Scheduler and Service Frame Budget
were removed (see below).

**Scope:** HDR output, Native HDR, Smooth Streaming, Script GC Scheduler and Service Frame Budget remain removed. Their findings are kept in `docs/removed-features.md`; do not restore them without a user request. Ambient Occlusion was reintroduced as standalone GTAO on 2026-09-30; see `docs/features/ambient-occlusion.md`. The earlier removal decision is historical.

## State of the code
- Frozen combined build (Apex inside a fork of S3SS): `%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\`, branch
  `night-remake`, tag `combined-final` (commit 45e36e2, local only). Full copy in
  `Backups Sims 3\16-antes da separacao (codigo completo)`. Treat it as read-only reference.
- This repository is Apex Radiance, the standalone ASI
  that can coexist with an unmodified official S3SS. The plan is `%USERPROFILE%\Desktop\S3SS-dev\PLANO-SEPARACAO.md`,
  summarised in `docs/architecture.md`. New work goes here. The framework is rewritten from scratch (no S3SS code).
- Current source paths refer to this repository. Explicitly historical sections retain combined-tree references.

## Read before touching anything
- Documentation follows `docs/DOCUMENTATION-STANDARD.md` (feature, validation and history pages) and the `apex-docs` skill; docs are updated once per release, in the pre-release review, not on every commit.
- Reusable workflows live in the repository: `.agents/skills/apex-docs`, `.agents/skills/apex-version-pr-workflow`, `.agents/skills/apex-compile-project`, `.agents/skills/apex-review-pr-release`; menu guidance and its audit live in `.agents/skills/apex-menu` (portable discovery) and `.claude/skills/apex-menu` (Claude Code discovery). Keep those two UI skill copies synchronized. Read the relevant skill when its task applies. Keep these paths relative so contributors on other machines can use them. New versions/features use a branch, small coherent commits and a draft PR. Coordinate simultaneous work in separate worktrees; merging and releasing remain explicit actions.
- `docs/README.md`: index. Then `docs/architecture.md` and `docs/workflow.md`.
- Before any lighting change: `docs/features/night-lighting/README.md`, the sub-part doc, and the engine docs
  (`docs/engine/`). Each feature doc has a "Rejected approaches" section linking to its history page. Do not retry what is
  listed there without new evidence.
- Raw sources, in Portuguese and chronological (later entries win): `S3SS-dev\NOTAS-ILUMINACAO.md`, `PASSO3-PLANO.md`,
  `ROADMAP-NIGHT-REMAKE.md`. Decompile: `S3SS-dev\re\out`. Game shaders: `Game\Bin\Shaders_Win32.precomp` (read-only).

## Build
MSBuild: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe`, v143,
Release|Win32, C++20, static CRT, vcpkg triplet `x86-windows-static` (always `/p:VcpkgEnableManifest=false`).

The public release is `Public\ApexRadiance.asi`, built with `ApexFlavorDefines=APEX_NO_DEV_TOOLS` and separate output
and intermediate directories. The maintainer's `Release\ApexRadiance.asi` retains developer tools and stays local.
Build with `ApexRadiance.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false` and the appropriate
flavor/output arguments. `ApexPublic` is a legacy no-op; see `build_flavor.h` and `docs/workflow.md`.
In the developer build, developer mode is off by default, chosen from `[ui] developer_mode` at startup before feature construction.
It is enabled in Settings > Menu after an explicit confirmation. Restart required for both directions.
The legacy `kPublicBuild` name now means normal runtime mode, not a compile-time flavor.
Developer instruments/threads/checks remain gated in that build; the public release omits them and never activates developer mode.
Profiles can optionally include Development. Show that save checkbox only when mode is requested/active;
show it on import when the profile contains it. Importing activation requires the same confirmation.
Profiles restore diagnostic preferences, never start recording/capture/profiling actions automatically.
See `docs/features/developer-mode.md` for persistence and verification details.

## Install (only with the game closed)
1. `TS3W.exe` and `Sims3LauncherW.exe` must both be closed (`tasklist | findstr /i "TS3W Sims3Launcher"`). Never kill
   them without asking.
2. Back up the installed `C:\Games\Hydra\The Sims 3\Game\Bin\ApexRadiance.asi` (the first time: the old
   `S3SSApex.asi`) into a new numbered folder `%USERPROFILE%\Desktop\Backups Sims 3\<NN-description>\` (next number: 84; 83 = sources of the indoor light between stories, work in progress kept out of the 1.5.1 release; 82 = installed asi before the indoor light between stories; 81 = sources before the indoor light between stories; 80 = installed asi before the glossy stair shader and the review fixes; 79 = installed asi before the flicker fix and the lamp switch updates; 78 = installed asi before the smooth indoor light; 77 = installed asi before the room light map edge padding; 76 = installed asi before the light probe alpha images; 75 = installed asi before the numbered light probe captures; 74 = light_probe.cpp before the numbered captures; 73 = installed asi before the terrain light map fix; 72 = lot_light_bridge.cpp before the terrain light map fix; 71 = installed asi before the 1.5.0 test; 70 = sources before the Night Lights brightness controls (1.5.0); 69 = installed asi before 1.4.8; 68 = installed asi before 1.4.7; 66 = sources before the Reset fix; 67 = installed asi before 1.4.6; 62-64 = picture.cpp and .claude.json before the color and MCP changes; 65 = installed asi before 1.4.5; 61 = installed 1.4.1 asi before the 1.4.2 test; 60 = installed 1.4.0 asi before 1.4.1; 59 = sources before the performance translation; 58 = sources before the lighting translation; 56 = resource_cache.cpp before the convert index fix; 57 = installed asi + crash report before it; 55 = installed asi before SceneNodeBudget was suspended; 54 = installed v1.8.0 asi before the crash reporter; 53 = installed asi before v1.8.0; 52 = sources before the v1.8.0 review fixes; 50 = sources before the v1.7.0 review fixes; 51 = installed asi before v1.7.0; 23 = source before the menu UX features; 42 = source before the compression features; 46 = source before the round 3 performance features; 47 = conflicted sources before the v1.5.0 merge of perf-c6-c8).
3. Copy the dev `Release\ApexRadiance.asi` into `Game\Bin\`. Exactly one copy of the mod in `Bin`: **delete the old
   `S3SSApex.asi`** (previous standalone and combined-build name). The official `Sims3SettingsSetter.asi` stays beside
   it. A leftover `S3SSApex.asi` is detected: if it loaded first Apex Radiance idles (log error only), otherwise the old
   one idles and a banner asks to delete it; the old combined build makes Apex Radiance keep its features off.
4. Config, log and dev outputs: `Documents\Electronic Arts\The Sims 3\Apex Radiance\` (`ApexRadiance.toml`,
   `ApexRadiance_LOG.txt`, `apex_radiance_imgui.ini`, `ApexRadiance_Hitches.txt`, `ApexRadiance_FrameCapture.txt`,
   `ApexRadiance_LightDiag.txt`, `ApexRadiance_LightProbe.txt` + `LightProbe\`, `ApexRadiance_Censo.txt` + `Censo\`,
   `Profiles\<name>.toml` = the menu's profiles, see `docs/ui.md`).
   First start without `ApexRadiance.toml`: copies the previous standalone's `...\S3SS\Apex\Apex.toml` as it is (old
   folder left in place; its `apex_imgui.ini` is not copied), else migrates from `...\S3SS\S3SS.toml` (backup
   `S3SS.toml.pre-split.bak` in the new folder). Official S3SS keeps `...\S3SS\` (`S3SS.toml`, `S3SS_LOG.txt`); Apex
   Radiance reads its configuration without modifying it.

## Rules from the user (always)
- **Back up before modifying** any game, mod, config or source file, into `Backups Sims 3\<numbered folder>`, never
  only the scratchpad.
- **No guessing.** Study before implementing. What works: F7 GPU probe, F8 diag, read the real shader and its
  constants, patch by pattern, test offline over all captured shaders, adversarial review. Mark unverified facts as
  unverified.
- **English is the source language** of UI, tooltips and status text; logs, the Developer page and file names stay
  English only. The menu is translated into 21 languages (PT/ES/FR in `i18n/tr_*.cpp`, the rest in `i18n/lang_<code>.cpp`; Settings > Menu >
  Language; automatic detection from the Windows language): write new texts in English and add their translations to the tables in
  `i18n/tr_*.cpp` (how: `i18n/TRANSLATING.md`; the widgets translate what they draw, raw ImGui text and run-time text
  need `I18n::Tr` / `Trf`). **The user chats in Portuguese; reply in Portuguese.**
- **Credits:** "Credits: @loinyx" only at the end of each feature description shown on hover; no visible credit lines
  outside the menu's Settings > Credits section, which lists sims3fiend (Sims3SettingsSetter, the model for the
  rewritten framework), the line "Every-Story Ground Light (lamps on upper floors lighting the ground) uses a technique from Arro's Split-Level Lighting Fix." (user-approved 2026-09-29; also in the README credits with a link to arro-now.tumblr.com; never in feature descriptions or promo text, and never implying the whole mod is based on it), FXAA and third-party code (ImGui, Detours, toml++, SMAA, Lucide icons ISC). The framework was rewritten, so
  there are no carried sims3fiend file headers. Sims3SettingsSetter is named only in compatibility notices, detection,
  the S3SS recommendation card, Credits and the S3SS.toml migration.
- **Name:** visible text says "Apex Radiance" via `APEX_PRODUCT_NAME`; never "S3SS Apex" / "Apex Edition" again.
- **Every new function (user rule, 2026-09-29):** after adding any feature or option, review the whole menu for the
  best organization (page, tab, card, group, order, labels that read alike, Advanced vs visible, Experimental badge,
  dependencies) and adjust what needs it, not only the new row. Use [the repository `apex-menu` skill](.agents/skills/apex-menu/SKILL.md)
  and run `perl .claude/skills/apex-menu/audit.pl .` from the repository root. It reports untranslated texts,
  copy-guideline breaks, undocumented keys and settings missing from ResetDefaults; treat these as heuristic reports
  and check runtime text and persistence manually. Report what was moved or renamed.
- **UI:** main controls visible, tuning in collapsed "Advanced", dev tools apart (Developer sections, public build
  hides them). New standalone UI theme: **Violet** (accent #7F77DD, dark #534AB7, light #CECBF6; window #15161a, cards
  #1c1d22), sidebar plus feature cards, own hotkey Ctrl+Shift+F11 (see `docs/ui.md`).
- **Git:** author the maintainer's git identity (loinyx). Never commit game shader bytecode (`*_ref.h`), dev
  leftovers (`patches/call_trace_patch.cpp`, `patches/lot_edge_lighting_patch.cpp`, `patches/light_diag_patch.cpp`,
  `trace_targets.h`), or `Public/`. Commit only when asked.
- **Publishing, pushing, releases, renames: only with the user's explicit OK.** Licensing is open (upstream has no
  license; the user should ask sims3fiend before the standalone is published).
- Offline test harnesses: read-only, never write many files from a test exe (Kaspersky flagged one as ransomware).

## Diagnosing
Ask what the user saw first. Then read `ApexRadiance_LOG.txt` (patterns found/installed; shaders that did not match;
`[Config] Migration path:`; S3SS / old-build detection), the feature's status lines, an F7 capture, the F8 diag, and
the Frame Profiler / `ApexRadiance_Hitches.txt` for performance. The docs quote the combined build's names
(`S3SS_LOG.txt`, `S3SS_Hitches.txt`, ...). See
`docs/workflow.md` section 4 and `docs/features/dev-tools/`.

## Release
- Every release requires `.agents/skills/apex-review-pr-release/SKILL.md` before publication: independent review of substantive code, evidence-based bug/regression debate, performance costs, preservation of intended visuals and documentation consistency. Record the exact reviewed/tested SHA; unresolved gameplay evidence remains unverified. Check changes added after the review before releasing. Publication is still explicitly authorized by the user.
Publish `Public\ApexRadiance.asi` to `loinyx/Sims3-ApexRadiance`, with English player-facing notes and an Install section.
Follow `docs/workflow.md` section 6 and the current release skill. The GitHub release triggers the Nexus upload workflow;
verify the published artifact's identity and the workflow result. The previous combined-build repository and
`S3SSApex.asi` releases are historical; see `docs/history/workflow.md`.

## Map of docs
- `docs/architecture.md`: hooks, registry priorities and Skip, post-scene chain (Edge 20, DepthBlur 30), INTZ depth
  share, patch system, TOML, logger, flavours, threads, standalone split.
- `docs/engine/*.md`: TS3W.exe RE with address tables (main loop, streaming, terrain light bake, room light maps,
  light objects and rigs, shaders, camera/map view, Mono GC, timers).
- `docs/features/**/*.md`: one per feature (Night Lighting and Performance are groups with one page per part): what it
  is, the problem, how it is solved, settings, limitations, technical reference. Tests are in `docs/validation/`,
  dated investigations and rejected approaches in `docs/history/`.
- `docs/ui.md`: the Violet menu (pages, widgets, startup banners).
- `docs/features/performance/`: the resource lookup cache (FindProvider, package list, database classes), the lot
  lighting budget while moving, the DXT encoders (0x006152F0 / 0x006154B0, reverse-engineered step by step) and the
  RefPack stream (compressor, decompressor, callers), the scene pending-node drain (0x006E4130) and the object tree
  walk behind the lookup by ID (0x00C62D40); `framework/slot_chain.h` shares vtable slots, `framework/entry_chain.h`
  function entries and `framework/call_chain.h` CALL instructions with the Frame Profiler.
- `docs/removed-features.md`: HDR, Native HDR, AO, Smooth Streaming, Script GC Scheduler, Service Frame Budget
  (revival notes).

## Local UI attribution decision (2026-10-02)
The user requested less Sims3SettingsSetter prominence. Do not restore its global footer detection label or the long promotional About paragraph. About leads with @loinyx and keeps only a compact sims3fiend framework-design credit. Compatibility detection remains in its own page; project historical attribution and licenses are retained.

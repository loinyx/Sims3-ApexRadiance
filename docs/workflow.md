# Workflow: build, install, rules, diagnosis, release

This page is the operating manual for working on Apex Radiance (formerly Sims3 Settings Setter Apex Edition). The
user's rules in section 3 are not optional.

## Version branches and pull requests

Before every release, apply [apex-review-pr-release](../.codex/skills/apex-review-pr-release/SKILL.md). Substantive code receives independent review and evidence-based discussion; fix demonstrated bugs/regressions, assess equivalence-preserving optimizations, preserve intended visuals and update documentation in its existing format. Record the exact reviewed/tested commit and unresolved gameplay/performance validation. Review relevant changes added afterwards before publishing. This is a release requirement, not authorization to merge or publish; artifact verification after publication must match the reviewed build.

New versions/features are developed on a branch with small coherent commits and a draft PR. Resume the existing PR across chats; keep its current scope, tested commit and remaining validation visible. Concurrent contributors use separate worktrees and contribution branches, with one integrator updating the shared PR. Fetch and inspect new remote commits before pushing; never overwrite another agent's work with a force push. Merge and release follow explicit user requests.

Reusable workflows are tracked in [.codex/skills/apex-version-pr-workflow](../.codex/skills/apex-version-pr-workflow/SKILL.md), [.codex/skills/apex-compile-project](../.codex/skills/apex-compile-project/SKILL.md), and [.codex/skills/apex-review-pr-release](../.codex/skills/apex-review-pr-release/SKILL.md). The UI workflow and portable audit are in [.agents/skills/apex-menu](../.agents/skills/apex-menu/SKILL.md), with a matching [Claude Code discovery copy](../.claude/skills/apex-menu/SKILL.md); keep both and their audit scripts in sync. The audit uses Perl and accepts the repository root as an argument. The compilation helper accepts a reviewed executable/argument recipe, verifies expected artifacts, saves full local logs and reuses successful commands. Skill caches/checkpoints and task scratch/output directories are ignored; no machine-specific recipes or build binaries are committed.

Paths used below:

| What | Path |
|---|---|
| Combined build (frozen, tag `combined-final`, commit 45e36e2, branch `night-remake`) | `%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\` |
| Standalone project, Apex Radiance | `%USERPROFILE%\Documents\Projetos\ApexRadiance\` |
| Game binaries (`TS3W.exe`, `Sims3LauncherW.exe`, ASIs, DXVK `d3d9.dll`) | `C:\Games\Hydra\The Sims 3\Game\Bin\` |
| Game shaders (read-only reference) | `C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp` |
| Mod config, log and dev outputs | standalone: `%USERPROFILE%\Documents\Electronic Arts\The Sims 3\Apex Radiance\`; combined build and official S3SS: `...\The Sims 3\S3SS\`; previous standalone (S3SSApex.asi, migrated from): `...\S3SS\Apex\` |
| Backups | `%USERPROFILE%\Desktop\Backups Sims 3\<NN-description>\` |
| Knowledge notes (Portuguese) | `%USERPROFILE%\Desktop\S3SS-dev\NOTAS-ILUMINACAO.md`, `PASSO3-PLANO.md`, `PLANO-SEPARACAO.md`, `ROADMAP-NIGHT-REMAKE.md` |
| Ghidra project and decompile output | `%USERPROFILE%\Desktop\S3SS-dev\re\` (`re\out` = decompiled C per function) |
| MSBuild | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe` |
| GitHub CLI | `C:\Program Files\GitHub CLI\gh.exe` (logged in as `loinyx`) |

The game setup: The Sims 3 Steam 1.67.2 in 4K on an RTX 4070 Ti SUPER, `d3d9.dll` = official DXVK 3.1.1 (sha256
starts `265888c3`; it was once misread as "2.0"), Ultimate ASI Loader 9.7.4 as `wininet.dll`, plus
`Sims3Performance.asi` and `MonoPatcher.asi`. Nothing in `Bin` except the mod's `.asi` should be changed without an
explicit request.

---

## 1. Build

Toolchain: Visual Studio 2022 Build Tools, toolset v143, C++20, Release|Win32, static CRT (`MultiThreaded`),
whole-program optimisation. Dependencies (imgui with dx9/win32 bindings, detours, tomlplusplus, mimalloc for the
combined build) come from vcpkg, triplet `x86-windows-static`, already installed in `vcpkg_installed\`; always pass
`/p:VcpkgEnableManifest=false` so MSBuild does not try to reinstall them.

### Combined build (frozen tree)

Run from `%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\` (commands from its `README.md`):

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" Sims3SettingsSetter.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" Sims3SettingsSetter.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false /p:S3SSPublic=true
```

| Flavour | Property | Define | Output | Contents |
|---|---|---|---|---|
| dev (default) | none | none (`kPublicBuild = false`) | `Release\S3SSApex.asi` (+ `.pdb`) | everything, plus the developer tools (Light Probe Ctrl+Shift+F7, Light Diag Ctrl+Shift+F8, Frame Capture Ctrl+Shift+F9, Lot Map Probe, census) and each feature's "Developer" section |
| public | `/p:S3SSPublic=true` | `S3SS_PUBLIC` (`kPublicBuild = true`) | `Public\S3SSApex.asi`, objects in `Public\obj\` | the dev tools and Developer sections compiled out |

Both flavours use English text only (`build_flavor.h`: `S3SS_TR(pt, en)` expands to `en`; it only remains so older call
sites compile). The two builds share sources; run them one after the other, not in parallel (they share
`vcpkg_installed` and the project's intermediate state for the dev flavour).

In PowerShell remember the call operator: `& "C:\Program Files (x86)\...\MSBuild.exe" ...`.

### Standalone (Apex Radiance)

From 2026-10-02 the standalone uses one binary, `Release/ApexRadiance.asi`, containing optional developer tools.
Build once with `ApexRadiance.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false`.
The previous `ApexPublic` property is a no-op. Do not distribute two flavors.
Settings > Menu > Enable developer mode defaults off, requires confirmation and applies after restarting the game.
The combined-tree flavor table above is historical. See [developer-mode.md](features/developer-mode.md).

### Offline checks before a game test

Shader-patch changes can be tested without the game: small read-only C++ harnesses in the session scratchpad include
`shader_patches.cpp` and run the patchers over every captured shader, disassembling the result with
`D3DDisassemble` (see [features/dev-tools/census.md](features/dev-tools/census.md)). `Shaders_Win32.precomp` holds every
game shader and is the reference for coverage (e.g. the 58 ExteriorWall pixel shaders in `wall_lamp_table.h`).
Warning: a test executable that wrote many files was flagged by Kaspersky as ransomware. Never write many files from a
test exe; print to stdout or write one file.

---

## 2. Install

1. **Close the game and the launcher.** Both `TS3W.exe` and `Sims3LauncherW.exe` must not be running (the launcher also
   holds `d3d9.dll` and files in `Bin`). Check with `tasklist | findstr /i "TS3W Sims3Launcher"`. Never copy into `Bin`
   while either is open; never kill the user's game without asking.
2. **Back up first** (rule 3.1): copy the currently installed `Bin\ApexRadiance.asi` (or, the first time, the old
   `Bin\S3SSApex.asi`) into a new numbered backup folder, e.g.
   `Backups Sims 3\20-<what is about to change>\ApexRadiance.asi.instalado-antes` (the naming the earlier folders use),
   and any source file you are about to modify (`<file>.antes-<change>`).
3. Copy the dev build `Release\ApexRadiance.asi` into `C:\Games\Hydra\The Sims 3\Game\Bin\` (the user plays the dev
   build; the public build is for releases and for friends).
4. Only one copy of the mod may be in `Bin`. **Remove the old `S3SSApex.asi`** (previous standalone name, and also the
   combined build's name). The official S3SS (`Sims3SettingsSetter.asi`) stays next to `ApexRadiance.asi`. If an old
   `S3SSApex.asi` is left behind, Apex Radiance detects it: when it loaded first, Apex Radiance stays idle and says so in
   its log; otherwise the old one stays idle and Apex Radiance shows a banner. The old combined build makes Apex Radiance
   refuse to start its features (banner). See [architecture.md](architecture.md), standalone section.
5. Settings are not in the `.asi`; they live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance.toml`.
   On the first start without it, Apex Radiance copies `...\S3SS\Apex\Apex.toml` (previous standalone) as it is, else
   migrates from `...\S3SS\S3SS.toml` (combined build). Some settings (borderless, anything read at CreateDevice) apply
   only from the next game start.
6. Tell the user (in Portuguese) what to test and what to look for.

Existing backup folders (2026-09-28): `9-S3SS compilado anterior`, `10-DXVK 3.1.1 oficial (original do jogo)`,
`11-antes da reorganizacao Apex`, `12-antes dos filtros Picture`, `13-antes do HDR nativo`, `14-antes da otimizacao`,
`15-borda-e-portao`, `16-antes da separacao (codigo completo)` (the full combined tree), `17-antes do Apex standalone`,
`18-antes da UI Violet`, `19-antes do rename Apex Radiance (codigo)` (standalone source before the rename), plus
`S3SS-source-2026-09-28-english-ui`. The folder vanished once (2026-09-26) and was recreated, so older backups (including
the vanilla S3SS) are gone. The next folder number is 20.

---

## 3. Standing rules from the user

1. **Always back up originals before modifying** anything (game files, mod files, configs, drivers, source files you
   are about to rewrite) into `%USERPROFILE%\Desktop\Backups Sims 3\<numbered folder>`, a permanent place, never only
   the scratchpad.
2. **No guessing: study before implementing.** Static deduction failed repeatedly on this project. The method that
   works: (a) GPU probe capture (Ctrl+Shift+F7), (b) F8 diag, (c) read the actual shader and its constants,
   (d) patch by pattern and test offline over all captured shaders, (e) adversarial review (review workflows found real
   bugs every time). Mark anything not verified as unverified, in code comments and in docs.
3. **English-only UI and text** in both builds: menus, tooltips, status lines, log lines, README. The user chats in
   **Portuguese**; answer in Portuguese.
4. **Credits:** "Credits: @loinyx" goes only at the end of each feature's description shown as a hover tooltip on the
   feature header. No visible credit lines elsewhere in the UI. Releases say "based on Sims3SettingsSetter by
   sims3fiend" and credit third parties (SMAA by Jimenez et al., MIT).
5. **UI organisation:** simple main controls visible; tuning controls inside collapsed "Advanced" sections; dev-only
   tools grouped apart (Developer sections, compiled out of the public build). Every new control gets English text, a
   hint, a sane default, and the credit at the end of its feature description.
6. **UI theme for the standalone: Violet** (accent #7F77DD, dark #534AB7, light #CECBF6) on a dark window (#15161a,
   cards #1c1d22). Layout agreed from a mockup: left sidebar with icons (Lighting, Image, Performance, Display, About;
   HDR was in the mockup but HDR is removed), feature cards (icon, name, one-line description, toggle), main sliders on
   the card, Advanced collapsed, a top status bar (day/night, frame ms / fps), own hotkey (plan: Ctrl+Shift+F11).
7. **Name:** "Apex Radiance" (full: "Apex Radiance for The Sims 3"; `APEX_PRODUCT_NAME` / `APEX_PRODUCT_TAGLINE` in
   `apex_version.h`), file `ApexRadiance.asi`. Formerly "Sims3 Settings Setter Apex Edition" / "S3SS Apex" /
   `S3SSApex.asi`. Sims3SettingsSetter is mentioned only in compatibility notices, detection, the S3SS recommendation
   card and the Credits section. Internal identifiers keep "Apex" (namespaces, `ApexPatch`, `APEX_` macros, `apex_*`
   source files).
8. **Git:** commits use the user's identity the maintainer's git identity (loinyx). Never commit:
   the game's shader bytecode (`*_ref.h`), the dev leftovers `patches/call_trace_patch.cpp`,
   `patches/lot_edge_lighting_patch.cpp`, `patches/light_diag_patch.cpp`, `trace_targets.h`, or the `Public/` output
   (already in `.gitignore`). Commit only when the user asks.
9. **Publishing, pushing, releases, repo renames: only with the user's explicit OK** (e.g. "publica").
10. **Scope decision 2026-09-28:** HDR output, Native HDR and Ambient Occlusion are removed from the standalone; only
    the Picture filters stay (SDR-only). See [removed-features.md](removed-features.md). Smooth Patch Precise
    improvements stay out of the standalone (upstream PR candidate).
11. **Licensing is open:** upstream S3SS has no license. The user should ask sims3fiend before publishing the standalone.
    The standalone's framework was rewritten from scratch (no S3SS code carried), so there are no sims3fiend file
    headers to keep; credits (sims3fiend, FXAA, third-party code including Lucide icons, and one line for Every-Story
    Ground Light) live in the menu's Settings > About > Credits section. That line, "Every-Story Ground Light uses a
    technique first shared by Arro.", is the only public mention: never name Arro or the Split-Level fix in feature
    descriptions, tooltips, release notes or promo text (functional notices about official S3SS's own fix being on stay).
12. New work goes into the standalone folder (`S3SSApex\`, Apex Radiance), not into the frozen combined folder.

---

## 4. Diagnosing a problem

Start by asking the user exactly what they saw (where, time of day, season, which lot, screenshot). Then:

1. **Log:** `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance_LOG.txt` (fallback:
   `Game\Bin\ApexRadiance_LOG.txt`; the combined build wrote `...\S3SS\S3SS_LOG.txt`). The `[Config] Migration path:`
   line tells how the settings were first created. Every patch
   logs whether its pattern was found and installed. Shader patchers log when a shader does not match the expected
   pattern (the old Portuguese message was "sem o padrao esperado"). The dev UI also shows per-feature status lines
   under Advanced/Developer > Status.
2. **Light Probe, Ctrl+Shift+F7 (dev):** GPU capture of the draws covering the pixel under the cursor: shaders,
   constants, textures, output `S3SS_LightProbe.txt` and the `LightProbe\` folder; archived captures are named
   `LightProbe-<name>` / `LightProbe-mNN` and are referenced throughout the notes. See
   [features/dev-tools/light-probe.md](features/dev-tools/light-probe.md).
3. **Light Diag, Ctrl+Shift+F8 (dev):** dumps the light manager state, lamps, per-pixel lamp section, level share
   decisions to `S3SS_LightDiag.txt` (writer: `light_diag.cpp`, not the untracked `patches/light_diag_patch.cpp`). See
   [features/dev-tools/light-diag.md](features/dev-tools/light-diag.md).
4. **Frame Capture, Ctrl+Shift+F9 (dev):** draw-by-draw list of 2 consecutive frames (`kFramesPerCapture = 2`) to `S3SS_FrameCapture.txt`. See
   [features/dev-tools/frame-capture.md](features/dev-tools/frame-capture.md).
5. **Census (dev):** `S3SS_Censo.txt`, which shader pairs are drawn and which have no correction; false colour mode.
   See [features/dev-tools/census.md](features/dev-tools/census.md).
6. **Performance:** the Frame Profiler ([features/frame-profiler.md](features/frame-profiler.md)) with per-service and
   per-hook timing, sampling, and `S3SS_Hitches.txt` for long frames.
7. Compare with the notes before theorising: most surfaces have a recorded capture and root cause
   ([features/night-lighting/README.md](features/night-lighting/README.md)).

The file names above are the combined build's, in the S3SS documents folder. Apex Radiance writes them to
`Documents\Electronic Arts\The Sims 3\Apex Radiance\` as `ApexRadiance_LOG.txt` (header "Apex Radiance Log"),
`ApexRadiance_LightProbe.txt` (+ `LightProbe\`), `ApexRadiance_LightDiag.txt`, `ApexRadiance_FrameCapture.txt`,
`ApexRadiance_Censo.txt` (+ `Censo\`) and `ApexRadiance_Hitches.txt`. The previous standalone build wrote `Apex_*.txt`
under `...\S3SS\Apex\` (left in place, not migrated).

---

## 5. Release process

### Current standalone release (2.5.6)

The repository is `loinyx/Sims3-ApexRadiance`; the combined-build information below is historical. Version 2.5.6 was published with the unified `Release/ApexRadiance.asi`, developer mode off by default and Optimize rendering on by default for missing settings. Explicit saved off choices are preserved. Build once and check translations. Future candidates remain private until publication is explicitly requested. Release notes use a short `##` headline, player-facing `- **Title:** description` bullets and `## Install`; the Nexus workflow derives its changelog from that body. Update source documentation and retain requested description approval before publication. Internal provenance notes are excluded from Git and source archives. The player acceptance of world-lamp response is documented in `features/night-lighting/world-lamp-response.md`; it is not an instrumented latency/FPS measurement. Documentation-only follow-ups do not require rebuilding or replacing the released ASI.

Repository: https://github.com/loinyx/Sims3SettingsSetter-Apex (public fork of sims3fiend/Sims3SettingsSetter; renamed
from `Sims3SettingsSetter-NightRemake` on 2026-09-28, old links redirect). It holds the **combined** build until the
standalone ships; the standalone may get its own repo (plan suggests e.g. `loinyx/S3SS-Apex`, not decided).

Local combined repo: branch `night-remake`, remote `fork` (URL already points to -Apex), remote `origin` = upstream.
The tag `combined-final` is local only (not pushed).

Steps, each only after the user's explicit OK:

1. Build both flavours; install and let the user test the dev build.
2. Update `README.md` (English only) and the changelog.
3. Commit with the user's identity (section 3.8), excluding the never-commit files.
4. Push (gh supplies the credentials):
   ```
   git -c credential.helper= -c 'credential.helper=!"/c/Program Files/GitHub CLI/gh.exe" auth git-credential' push fork night-remake:main
   ```
5. Create the release with the **public** build as the asset:
   ```
   "C:\Program Files\GitHub CLI\gh.exe" release create <tag> "Public\S3SSApex.asi" -R loinyx/Sims3SettingsSetter-Apex --title "<title>" --notes-file <notes.md>
   ```
   (That is the combined build's asset. A standalone release ships `Public\ApexRadiance.asi`; its repository and any
   repo rename are not decided and need the user's explicit OK.)

Existing releases:

| Tag | Title | Date | Notes |
|---|---|---|---|
| `nightremake-v0.1.0-alpha` | Night Remake 0.1.0 alpha | 2026-09-27 | first release, asset was `Sims3SettingsSetter.asi` |
| `apex-v0.2.0-alpha` | Sims3 Settings Setter Apex Edition v0.2.0-alpha | 2026-09-28 | marked Latest; asset `S3SSApex.asi`; HDR, SMAA, deterministic AO, Apex/Display tabs, Depth Blur off in map view, Split-Level Lighting Fix removed |

Release-notes shape used for v0.2.0 (keep it): title line, one-paragraph summary crediting @loinyx and "Unofficial fork
of Sims3SettingsSetter by sims3fiend"; sections **New**, **Menu**, **Notes** (requirements such as "needs the game's own
Edge Smoothing off"), **Install** (close the game, delete old file names, copy the `.asi` into `The Sims 3\Game\Bin`,
settings are kept in `Documents\Electronic Arts\The Sims 3\S3SS`), and a credits line
("Credits: @loinyx · based on Sims3SettingsSetter by sims3fiend · SMAA by Jimenez et al. (MIT)").

Packages for the user's friend were zipped on the Desktop / in the scratchpad (`pacote-night-remake-v5.x-{dev,public}`);
the friend gets the public build unless asked otherwise.

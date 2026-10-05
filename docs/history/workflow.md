# Workflow: history

Chronological record of the build, install and release process before the current one. The current process is
described in [workflow.md](../workflow.md).

### 2026-09-27: combined build and its two flavours

**Context:** until the standalone split, Apex lived inside a fork of Sims3SettingsSetter, built from
`%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\` (branch `night-remake`, frozen as tag `combined-final`, commit
`45e36e2`, local only). Dependencies also included mimalloc for S3SS's allocator hook.

**Finding:** the tree was built twice, one flavour after the other (they share `vcpkg_installed` and intermediate
state), with the commands from its `README.md`:

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" Sims3SettingsSetter.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" Sims3SettingsSetter.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false /p:S3SSPublic=true
```

| Flavour | Property | Define | Output | Contents |
|---|---|---|---|---|
| dev (default) | none | none (`kPublicBuild = false`) | `Release\S3SSApex.asi` (+ `.pdb`) | everything, plus the developer tools (Light Probe Ctrl+Shift+F7, Light Diag Ctrl+Shift+F8, Frame Capture Ctrl+Shift+F9, Lot Map Probe, census) and each feature's "Developer" section |
| public | `/p:S3SSPublic=true` | `S3SS_PUBLIC` (`kPublicBuild = true`) | `Public\S3SSApex.asi`, objects in `Public\obj\` | the developer tools and Developer sections compiled out |

Both flavours used English text only (`build_flavor.h`: `S3SS_TR(pt, en)` expands to `en`, kept so older call sites
compile). The maintainer played the dev build; the public build was for releases and for a friend, who received zipped
packages `pacote-night-remake-v5.x-{dev,public}` from the Desktop or the scratchpad (the public build unless asked
otherwise).

**Outcome:** replaced by the standalone's single build. The standalone first kept two flavours (`/p:ApexPublic=true`
defined `S3SS_PUBLIC` and wrote `Public\ApexRadiance.asi`); on 2026-10-02 it moved to one unified binary with an
optional developer mode, and `ApexPublic` became a no-op.

### 2026-09-28: backup folders at the split

**Context:** inventory of `%USERPROFILE%\Desktop\Backups Sims 3\` when the standalone started.

**Finding:** existing folders: `9-S3SS compilado anterior`, `10-DXVK 3.1.1 oficial (original do jogo)`,
`11-antes da reorganizacao Apex`, `12-antes dos filtros Picture`, `13-antes do HDR nativo`, `14-antes da otimizacao`,
`15-borda-e-portao`, `16-antes da separacao (codigo completo)` (the full combined tree), `17-antes do Apex standalone`,
`18-antes da UI Violet`, `19-antes do rename Apex Radiance (codigo)` (standalone source before the rename), plus
`S3SS-source-2026-09-28-english-ui`. The folder vanished once (2026-09-26) and was recreated, so older backups,
including the vanilla S3SS, are gone. The next number was then 20.

**Outcome:** numbering continues; the current next number is kept in `CLAUDE.md`.

### 2026-09-28: combined-build release process

**Context:** releases of the combined build went to `loinyx/Sims3SettingsSetter-Apex`, a public fork of
`sims3fiend/Sims3SettingsSetter`, renamed from `Sims3SettingsSetter-NightRemake` on 2026-09-28 (old links redirect).
Local repository: branch `night-remake`, remote `fork` (pointing to -Apex), remote `origin` = upstream.

**Finding:** steps, each after the maintainer's explicit approval:

1. Build both flavours; install and test the dev build.
2. Update `README.md` (English only) and the changelog.
3. Commit with the maintainer's identity, excluding the never-commit files.
4. Push, with gh supplying the credentials:
   ```
   git -c credential.helper= -c 'credential.helper=!"/c/Program Files/GitHub CLI/gh.exe" auth git-credential' push fork night-remake:main
   ```
5. Create the release with the public build as the asset:
   ```
   "C:\Program Files\GitHub CLI\gh.exe" release create <tag> "Public\S3SSApex.asi" -R loinyx/Sims3SettingsSetter-Apex --title "<title>" --notes-file <notes.md>
   ```

| Tag | Title | Date | Notes |
|---|---|---|---|
| `nightremake-v0.1.0-alpha` | Night Remake 0.1.0 alpha | 2026-09-27 | First release; asset `Sims3SettingsSetter.asi` |
| `apex-v0.2.0-alpha` | Sims3 Settings Setter Apex Edition v0.2.0-alpha | 2026-09-28 | Marked Latest; asset `S3SSApex.asi`; HDR, SMAA, deterministic AO, Apex/Display tabs, Depth Blur off in map view, Split-Level Lighting Fix removed |

Release-notes shape used for v0.2.0: title line; a one-paragraph summary crediting @loinyx and "Unofficial fork of
Sims3SettingsSetter by sims3fiend"; sections **New**, **Menu**, **Notes** (requirements such as "needs the game's own
Edge Smoothing off"), **Install** (close the game, delete old file names, copy the `.asi` into `The Sims 3\Game\Bin`,
settings kept in `Documents\Electronic Arts\The Sims 3\S3SS`); and a credits line ("Credits: @loinyx · based on
Sims3SettingsSetter by sims3fiend · SMAA by Jimenez et al. (MIT)").

**Outcome:** superseded. The standalone's repository was undecided at the time (the plan suggested names such as
`loinyx/S3SS-Apex`); Apex Radiance is published from `loinyx/Sims3-ApexRadiance` with the process in
[workflow.md](../workflow.md#6-release).

### 2026-09-28: early project rules that changed

**Context:** the rule list at the split differed from today's in three places.

**Finding:**

- **UI theme:** the Violet layout was agreed from a mockup with a left icon sidebar (Lighting, Image, Performance,
  Display, About; HDR was in the mockup but HDR was removed), feature cards (icon, name, one-line description,
  toggle), main sliders on the card, Advanced collapsed, a top status bar (day/night, frame ms / fps) and its own
  hotkey (planned Ctrl+Shift+F11).
- **Language:** the UI was English only in both builds until 1.4.1 added Portuguese, Spanish and French.
- **Scope:** HDR output, Native HDR and Ambient Occlusion were removed from the standalone. Ambient Occlusion came back
  as standalone GTAO on 2026-09-30.
- **Credits:** the Every-Story Ground Light line first read "uses a technique first shared by Arro"; it was replaced
  on 2026-09-29.

**Outcome:** the current rules are in [workflow.md](../workflow.md#3-project-rules) and the current layout in
[ui.md](../ui.md).

### 2026-10-02: unified build

**Context:** two standalone flavours (player and developer) had to be built, installed and released separately.

**Finding:** the developer tools can be gated at run time by a setting read before the features are created.

**Outcome:** one `Release\ApexRadiance.asi` with developer mode off by default
([features/developer-mode.md](../features/developer-mode.md)). Version 2.5.5 was the first release built this way;
2.5.6 additionally shipped Optimize rendering on by default for missing settings, preserving saved off choices
(pull request #2 removes that switch).

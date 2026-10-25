# Guidance for automated contributors

This file is for coding assistants working in this repository. Human contributors start with
[CONTRIBUTING.md](CONTRIBUTING.md); everything here also applies to them.

## Project

Apex Radiance is a native ASI mod for The Sims 3 (Steam 1.67.2, `TS3W.exe`, 32-bit). It hooks the Direct3D 9 device
and patches game code in memory (Detours, pattern scans), with an ImGui menu and TOML configuration. It runs beside an
unmodified Sims3SettingsSetter and does not depend on it. Visible names come from `apex_version.h`
(`APEX_PRODUCT_NAME`); internal identifiers keep the `Apex` prefix.

## Read first

- [docs/README.md](docs/README.md), then [docs/architecture.md](docs/architecture.md) and [docs/workflow.md](docs/workflow.md).
- The feature, validation and history pages of the area being changed. Each feature page lists rejected approaches;
  do not retry one without new evidence.
- Before any lighting change: `docs/features/night-lighting/README.md`, the part's page and `docs/engine/`.
- Documentation rules: [docs/DOCUMENTATION-STANDARD.md](docs/DOCUMENTATION-STANDARD.md).
- Anything that becomes public (code comments, docs, commit messages, PR titles and bodies, release notes):
  [docs/PUBLIC-TEXT-POLICY.md](docs/PUBLIC-TEXT-POLICY.md), checked by `python tools/public_text_check.py`.

## Implementation approach

Apply [Ponytail](.agents/skills/ponytail/SKILL.md) in full mode to coding tasks: trace the affected flow, reuse existing
code and make the smallest complete correction with appropriate tests. The lighting, build, installation and release
rules in this project remain authoritative.

## Rules

- **Evidence before patches.** Study the real shader, its constants and the engine code before implementing; patch by
  pattern; test offline over every captured shader. Mark unverified facts as unverified in code and docs.
- **English is the source language** of the UI, tooltips, status text, logs and file names. The menu is translated
  into 21 languages: add every new text to `i18n/tr_*.cpp` and the `i18n/lang_<code>.cpp` tables
  ([i18n/TRANSLATING.md](i18n/TRANSLATING.md)), then run `perl tools/i18n_keys.pl` and `tools/i18n_check`.
- **Menu changes** follow the `apex-menu` skill (`.agents/skills/apex-menu`, with a discovery copy in
  `.claude/skills/apex-menu`; keep them identical). After adding a feature or option, review the whole menu and run
  `perl tools/menu_audit.pl .`.
- **Credits** follow the README Credits section: "Credits: @loinyx" ends each feature description shown on hover; other
  credits live only in Settings > Credits and the README.
- **Never commit** game shader bytecode (`*_ref.h`), the untracked developer leftovers listed in `.gitignore`, build
  output (`Public/`, `Release/`) or captures.
- **Build:** [docs/workflow.md](docs/workflow.md#1-build). Public releases use `ApexFlavorDefines=APEX_NO_DEV_TOOLS`.
- **Releases** follow [docs/release-process.md](docs/release-process.md).
- Merging, publishing releases, pushing to `main` and renaming anything public require the maintainer's approval.
- Offline test harnesses are read-only and never write many files.
- **Public text** is reviewed against [docs/PUBLIC-TEXT-POLICY.md](docs/PUBLIC-TEXT-POLICY.md) before it is merged or
  published: standard shape, concise, no private context.

## Local notes

Machine-specific settings (paths, backup locations, the test machine, the maintainer's language) belong in an
untracked `CLAUDE.local.md` or `AGENTS.local.md` next to this file; both are ignored by Git.

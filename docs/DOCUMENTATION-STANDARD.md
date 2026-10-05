# Documentation standard

This standard defines how Apex Radiance is documented. It applies to every file under `docs/`. Templates live in
[templates/](templates/).

## 1. Goals

The documentation answers three questions, in this order:

1. **What is it?** What the feature does and what the player sees.
2. **Why is it needed?** The problem in the unmodified game.
3. **How does Apex Radiance solve it?** The approach first, then the technical detail a maintainer needs.

Test results, investigation diaries and release candidates are kept separately (section 2), so a feature page stays a
stable description of the current behaviour.

## 2. Document types and locations

| Type | Location | Answers | Changes when |
|---|---|---|---|
| Overview | `docs/README.md` | What the mod is, how the docs are organised | A feature is added or removed |
| Feature | `docs/features/<feature>.md` | What it is, the problem, the solution, settings, limits, technical reference | The feature's behaviour changes |
| Feature group | `docs/features/<group>/README.md` | Overview of a feature made of several parts (Night Lighting) | A part is added or removed |
| Engine reference | `docs/engine/<topic>.md` | How the game works internally (reverse engineering, addresses) | New evidence about the game |
| Validation | `docs/validation/<feature>.md` | How the feature is tested, latest results, open checks | A test is added or re-run |
| History | `docs/history/<feature>.md` | Dated investigations, rejected approaches with evidence, superseded designs | Append only |
| Release notes | `docs/releases/<version>.md` | What changed in a published version | Once per release |
| Architecture and workflow | `docs/architecture.md`, `docs/workflow.md` | Framework internals, build, install, diagnosis, release process | The framework or process changes |

One feature, one page per type. A feature page links to its validation and history pages; these never duplicate it.

## 3. Writing rules

- **Language:** English, present tense, describing the current code. Write "Depth Blur skips loading screens", not
  "Depth Blur now skips loading screens since PR #2".
- **Audience:** a modder or maintainer who knows Direct3D 9 basics but not this codebase. Explain a term the first time
  it appears or link to where it is explained.
- **Lead with the result.** The first paragraph describes what the player sees. Implementation detail comes after.
- **No dates, session names or build candidates in feature pages.** Dates belong in history, validation and release
  notes. Exception: the *Status* table records the version that introduced the feature.
- **Write as product documentation, not a conversation log.** Never write "at the user's request", "the user
  asked", "user-approved", "as requested", "we tried", "I", "you asked" or "Claude/Codex". State the behaviour and,
  where needed, the design reason ("Reset buttons are per control to avoid accidental page resets").
- **No conversation quotes** ("ficou incrível", "the user said"). Record the decision and, in history, the reason.
- **No test counts in feature pages.** "Verified with 9,400 checks" goes to the validation page.
- **Facts are verifiable.** Name the file, function, address or capture behind a claim. Mark anything not confirmed
  in game as *Unverified* (validation page) and say so in *Limitations* when it affects players.
- **Link, do not repeat.** A fact lives in one place. Other pages link to it.
- **Settings are documented once**, in the feature page's settings table, with the menu label, TOML key, type,
  default, range and effect. Defaults must match the code.
- **Units and numbers:** metres (m), milliseconds (ms), percentages for user-facing sliders, hexadecimal addresses as
  `0x00ABCDEF` with the game build stated (Steam 1.67.2 unless noted).
- **Code identifiers** in backticks; file references as relative links (`[ambient_occlusion_patch.cpp](../patches/ambient_occlusion_patch.cpp)`).
- **Length:** a feature page should read in five minutes. When the technical reference grows beyond a screen or two,
  move it to an engine page or a sub-page and link it.
- **Headings** follow the template exactly, so every page has the same shape. Omit a section only when it would be
  empty and the template marks it optional.

## 4. Feature page structure

Every feature page uses [templates/feature.md](templates/feature.md):

1. Title and one-paragraph summary (player view).
2. **Status** table: availability, default, menu location, TOML section, developer-mode only or not, main source files.
3. **The problem**: what the unmodified game does and why it looks or performs worse.
4. **How Apex Radiance solves it**: the approach in plain language, then the processing steps.
5. **Settings**: one table.
6. **Compatibility and interactions**: other features, other mods, game options, hardware requirements.
7. **Limitations**: known gaps a player could notice, and conditions where the feature turns itself off.
8. **Technical reference**: hooks, shaders, registers, resources, addresses, performance cost.
9. **Rejected approaches** (short list, each one line with a link to history).
10. **See also**: validation page, history page, related engine pages.

Engine reference pages use [templates/engine.md](templates/engine.md): summary, scope table (game build, features that
use it, evidence), overview, details, address reference, see also.

## 5. Validation page structure

Every validation page uses [templates/validation.md](templates/validation.md):

1. Scope: what the feature must do, stated as checkable properties.
2. **Automated tests**: each harness with its location, what it covers, how to run it, and whether it needs the GPU.
3. **Latest results**: a table of runs (date, commit, harness, result, backend).
4. **In-game test plan**: numbered scenarios a player or tester can repeat, with the expected result.
5. **Confirmed in game**: what was visually confirmed, on which build.
6. **Open checks**: what remains unverified.

Rules for harnesses are in [workflow.md](workflow.md): read-only, never write many files, never install into the game.

## 6. History page structure

History pages are chronological logs and may keep the original dates and evidence. Each entry starts with a level-3
heading `### YYYY-MM-DD: short title`, then *Context*, *Finding* and *Outcome* (kept, changed, or rejected and why).
Feature pages summarise rejected approaches in one line and link here.

## 7. Status vocabulary

Use these words only, in the Status table and in the README index:

| Status | Meaning |
|---|---|
| Released | In a published version. Give the version. |
| In development | On a branch or draft PR, not published. Give the PR. |
| Developer mode | Only available with developer mode on. |
| Experimental | Released but marked Experimental in the menu. |
| Removed | No longer in the code. Kept in `removed-features.md`. |

## 8. Review checklist

Before merging a documentation change:

- [ ] The page follows its template and heading order.
- [ ] The first paragraph says what the player sees.
- [ ] Settings table defaults match the code (`Params`, `Register*Setting`).
- [ ] No dates, test counts or quotes in a feature page outside the Status table.
- [ ] New test evidence is in the validation page; investigations are in the history page.
- [ ] Links resolve (relative paths).
- [ ] `docs/README.md` lists the page with the right status.

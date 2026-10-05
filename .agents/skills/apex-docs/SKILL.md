---
name: apex-docs
description: Write or update Apex Radiance documentation in the project standard (feature, validation, history, engine and release pages). Use once per release, during the pre-release review, or when explicitly asked to update the docs; not on every commit.
---

# Apex Radiance documentation

The standard is `docs/DOCUMENTATION-STANDARD.md`. Templates are in
`docs/templates/`. The finished example is the Ambient Occlusion set: `docs/features/ambient-occlusion.md`,
`docs/validation/ambient-occlusion.md`, `docs/history/ambient-occlusion.md`. Read the standard and the example before
writing.

## Where each fact goes

| Fact | Page |
|---|---|
| What the feature does, the problem, the solution, settings, limits, technical reference | `docs/features/<feature>.md` |
| Harnesses, how to run them, results with date and commit, in-game test plan, confirmed and open checks | `docs/validation/<feature>.md` |
| Dated investigations, superseded candidates, rejected approaches with evidence | `docs/history/<feature>.md` (append only) |
| How the game works internally | `docs/engine/<topic>.md` |
| What changed in a published version | `docs/releases/<version>.md` |

Grouped features (Night Lighting, Performance) use `docs/features/<group>/<part>.md` and
`docs/validation/<group>-<part>.md`, `docs/history/<group>-<part>.md`.

## When

Documentation is updated once per release, during `$apex-review-pr-release`, not on every commit. The source of
truth for what changed is the release diff plus the test results and open checks recorded in commit messages and the
PR description's handoff section.

## Workflow

1. Identify the affected features from the release diff (`git diff <last-release-tag>...HEAD --stat`) and collect the
   recorded test results from commit messages and the PR description.
2. For each one, update the feature page so it describes the code at HEAD. Check every settings row against the code:
   the `Params` struct, `Register*Setting` calls and `ResetDefaults`. Menu labels come from the UI code.
3. Put new test evidence in the validation page: add a row to *Latest results* with date, commit, harness, result and
   backend. Move fixed items out of *Open checks*; add new unverified items.
4. Append investigations and rejected approaches to the history page. Add a one-line entry to the feature page's
   *Rejected approaches* list when a design option was ruled out.
5. A new feature gets all three pages from the templates and a row in `docs/README.md` with its status.
6. Run the review checklist in section 8 of the standard and check that every relative link resolves.

## Tone

Professional product documentation in English, present tense. Never write as a conversation log: no "the user asked",
"at the user's request", "user-approved", quotes, "we/I", or agent names. State the behaviour and, when useful, the
design reason. No dates, test counts or build-candidate names in feature pages outside the Status table.

## Do not

- Do not delete history to tidy a page. Move it to the history page.
- Do not describe unverified gameplay as confirmed. Unconfirmed items stay in *Open checks* and, when players could
  notice them, in the feature page's *Limitations*.
- Do not duplicate a fact across pages. Link to it.

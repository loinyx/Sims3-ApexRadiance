---
name: apex-menu
description: Plan or review Apex Radiance in-game menu changes. Use when adding or changing a setting, feature card, menu copy, translation, or before reviewing a release that changes the UI.
---

# Apex Radiance menu

Read only the relevant sections of `docs/ui.md` and the affected feature documentation. Preserve its page and card structure, English source copy, translations, search behavior, settings reset, profile persistence, and documentation. Review the whole affected card after changing it; don't add a page for one setting or repeat nearby labels.

## Adding or changing a setting

- Place it on the page and in the card that match what the player controls. Keep common controls visible; put infrequent tuning in the existing advanced area and developer-only controls behind the developer UI.
- Use the existing `ApexUi` row widgets, stable TOML keys, documented defaults and ranges. Check enabled/disabled dependencies, changed indicators, reset behavior, profiles, presets, and light styles where applicable.
- Write UI copy in English and add Brazilian Portuguese, Spanish, and French entries in `i18n/tr_*.cpp`. Check raw ImGui and runtime text as well as translated row widgets.
- Update the feature's settings table and `docs/ui.md` when the layout changes. Update player-facing release material only when the change affects players.
- Check how the row participates in menu search (`SearchParts`) and how the same card reads as a whole.

## Icons

- Give each new feature or standalone card its own Lucide icon; do not reuse the parent feature's icon or an unrelated card's icon.
- Check the vendored `third_party/lucide/icons/` set first. If the icon is absent, add its SVG stroke elements to `ui/lucide_data.h` and its `IconId` to `ui/icons.h` in matching order; retain the Lucide name and license attribution.
- Review where the icon is used so it remains distinctive, and verify the enum/data count assertion with a build.

## Audit

Run the read-only audit from any directory, passing the repository root when it cannot be inferred from the script location:

```sh
perl .claude/skills/apex-menu/audit.pl .
```

It reports missing translation entries, copy guideline breaks, undocumented settings, and settings omitted from a local `ResetDefaults()`. It is a heuristic: manually check runtime/generated text, reset paths outside the feature file, profile and preset behavior, and whether each report is applicable. Developer-only English is allowed; investigate other findings rather than blindly changing text. Run it again after UI changes.

Use the project's current build instructions and applicable checks for the changed code; do not assume separate development and public binaries exist. Report the menu placement and any copy, labels, or layout changed, along with checks and unresolved findings.

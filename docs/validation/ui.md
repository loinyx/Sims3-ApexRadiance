# Menu UI: validation

The menu is described in [ui.md](../ui.md). It must:

- Stay closed, with no shortcut or notice, until a world is loaded and settled, and close again during a load.
- Draw every page in all four languages and at every text size without clipped text, missing translations or ImGui
  assertions.
- Keep controls of the same role at the same height, with popups restoring the parent's style.
- Never change a saved key, preset or setting by opening a page.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/loading_gate_test`](../../tools/loading_gate_test/run.ps1) | The real `WorldSession` reader and the extracted startup hint and Depth Blur loading guards against mocked game state | `run.ps1` | x86 MSVC |
| [`tools/report_check/overlay_check.cpp`](../../tools/report_check/overlay_check.cpp) | Overlay clock and the extracted menu availability gate against simulated load and unload | [README](../../tools/report_check/README.md) | x86 MSVC, ImGui |
| [`tools/report_check/control_layout_check.cpp`](../../tools/report_check/control_layout_check.cpp) | Shared control geometry and the profile picker: popup style stacks, adjacent heights, text ink centres, EN/PT/ES/FR at two scales | README | x86 MSVC, ImGui |
| [`tools/report_check/menu_253_check.cpp`](../../tools/report_check/menu_253_check.cpp) | The Report page and the notice layout (24 cases: four languages, three widths, two font sizes) | README | x86 MSVC, ImGui |
| [`tools/i18n_check`](../../tools/i18n_check/i18n_check.cpp) | Translation tables: conflicting duplicates, missing languages, placeholder mismatches | Build line in the source header | x86 MSVC |
| [`tools/s3pp_settings_test`](../../tools/s3pp_settings_test/run.ps1) | The `Sims3Performance.ini` reader and the two-line change behind the Compatibility item (sections, case, comments, duplicates, unchanged bytes); in the Smoke suite | `run.ps1` | x86 MSVC |
| [`tools/menu_audit.pl`](../../tools/menu_audit.pl) | Heuristic audit: untranslated texts, copy-guideline breaks, undocumented keys, settings missing from ResetDefaults | `perl tools/menu_audit.pl .` | Perl |
| [`tools/developer_mode_check`](../../tools/developer_mode_check/check.cpp) | Profile part bits and filtering | See [validation/developer-mode.md](developer-mode.md) | x86 MSVC |

Native checks are geometric evidence; they do not replace inspecting every page in the game.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-29 | `3839ebc` (2.18.0 release) | `tools/i18n_check` (built as a DLL: new executables were blocked on the test machine), `menu_audit.pl`, Release x86 build, in-game start | OK, 2426 of 2426 keys in every language; menu audit: 4 copy guideline notes, all older than this release; the footer shows 2.18.0 and What's new lists the 2.18.0 lines | Windows, DXVK |
| 2026-10-28 | `28dc5b3` (2.17.2 release) | `tools/i18n_check`, CPU Smoke suite with `tools/s3pp_settings_test`, `menu_audit.pl`, Release and public x86 builds | OK, 2340 of 2340 keys; Smoke passed, 10 of 10 ini checks; no audit finding on the new texts; both built | MSBuild / None |
| 2026-10-22 | `715a410` (the Tester labels *Window light channel memo* and *Paced relight while moving*) | `tools/i18n_check` | OK, 2243 of 2243 keys | None |
| 2026-10-03 | PR #2 (control sizing) | `control_layout_check` | 40 multi-frame cases passed across EN/PT/ES/FR and scales 1.0 / 1.4: popup style stacks, stable neighbouring heights, button glyph centres within 1.1 px, one-pixel divider origins | None |
| 2026-10-03 | PR #2 (control sizing) | `menu_253_check` | 24 notice and 40 page cases passed | None |
| 2026-10-03 | PR #2 (control sizing) | Release x86 build | Zero warnings | MSBuild |
| 2026-10-02 | Display runtime removal candidate (SHA-256 `75EFF7A869E709D9D1C9698FF00CEC5987D693F69FF2E43DA36BF2D20F8B203B`) | Release x86 build, translation tables, legacy profile filtering | Passed | MSBuild / None |
| 2026-10-02 | 2.5.4 hotfix | Four-language translation checks | Passed | None |

## In-game test plan

1. Start the game and wait on the main menu. **Expected:** no hint, the menu key does nothing.
2. Load a household. **Expected:** about three seconds after the loading screen closes, "Apex Radiance is ready · press
   <key>" appears for about eight seconds; the menu key opens the menu.
3. Travel to another world or return to the main menu. **Expected:** the menu closes during loading and the shortcuts do
   nothing until the new world settles.
4. Visit every page at text sizes 80%, 100% and 200% in each language. **Expected:** no clipped or overlapping text;
   controls of one role share a height; the sidebar collapses to icons and back.
5. Search "night lights", "blur" and a translated word. **Expected:** live rows with breadcrumbs; clicking a breadcrumb
   opens the page and clears the search.
6. Change a slider, then Undo from the toast. **Expected:** the previous value returns; the changed dot and the hover
   Reset work on that row.
7. Settings > Shortcuts: choose each preset, record a custom key, try Ctrl+Shift+C and bare F10. **Expected:** presets
   change only on selection; refused keys show the reason; Custom keeps unchanged actions on the previous preset.
8. Open the cheat console with Ctrl+Shift+C and type a cheat containing `c`. **Expected:** no screenshot and no other
   Apex shortcut until Enter or Esc.
9. Save a profile with an icon and a few parts, apply it with some parts unchecked. **Expected:** only the checked parts
   change; Undo restores the previous setup.
10. Hold Alt and B over the menu. **Expected:** the menu becomes see-through; Picture is bypassed while B is held.

## Confirmed in game

- 2026-10-28, `b206084` and `f8a90b8` (DXVK, Sims 3 Modernization 0.36.37, Sims 3 Performance Patch 1.0.0-beta2 with
  `ShortWaitSpin` and `ThreadStackDiet` on): the question showed once after the world loaded, with the menu closed;
  **Turn off** changed only those two lines to 0, kept the old file as `Sims3Performance.ini.apex-backup` and set
  `[ui] s3pp_spin_asked`; the log recorded the detection and the change. Shown on the first build's texts and again on
  the revised ones.
- 2.5.5 and 2.5.6: the menu layout, Report page and Settings tabs were used in play; the control sizing went through two
  revisions after in-game feedback (36/44, then 34/40, then 30/36 units).

## Open checks

- Startup hint timing after the world loads (PR #2), on EA and Steam installs, with Night Lights on and off.
- Final in-game screenshot review of the 30/36 control sizes, the profile selection card and the Shortcuts tab.
- Notice priority and the 140 / 120 ms animation at high text sizes.
- The "Reset all settings" description still mentions "window mode"; there is no window mode any more (code text to
  update).

## Public test notice

`tools/public_test_notice_test/run.ps1` extracts the production modal and uses real Violet widgets, fonts, icons and
translation tables with in-memory settings. It checks window/button bounds, style-stack restoration, multi-frame
stability, actual Continue clicks, versioned acknowledgment and full version labels in 21 languages at two widths and
two scales. The translation checker verifies all table keys and placeholders. Native headless geometry does not verify
live game input capture, saved-config persistence after process exit or gameplay rendering.

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
| [`.claude/skills/apex-menu/audit.pl`](../../.claude/skills/apex-menu/audit.pl) | Heuristic audit: untranslated texts, copy-guideline breaks, undocumented keys, settings missing from ResetDefaults | `perl .claude/skills/apex-menu/audit.pl .` | Perl |
| [`tools/developer_mode_check`](../../tools/developer_mode_check/check.cpp) | Profile part bits and filtering | See [validation/developer-mode.md](developer-mode.md) | x86 MSVC |

Native checks are geometric evidence; they do not replace inspecting every page in the game.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
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

- 2.5.5 and 2.5.6: the menu layout, Report page and Settings tabs were used in play; the control sizing went through two
  revisions after in-game feedback (36/44, then 34/40, then 30/36 units).

## Open checks

- Startup hint timing after the world loads (PR #2), on EA and Steam installs, with Night Lights on and off.
- Final in-game screenshot review of the 30/36 control sizes, the profile selection card and the Shortcuts tab.
- Notice priority and the 140 / 120 ms animation at high text sizes.
- The "Reset all settings" description still mentions "window mode"; there is no window mode any more (code text to
  update).

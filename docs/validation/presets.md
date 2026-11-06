# Presets: validation

[Presets](../features/presets.md) must preserve existing TOML settings, reject malformed packages and share a selected LUT without overwriting an unrelated file.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/profile_package_test/` | Package parsing, CRC, paths, collisions, rollback and production import/export backend | See harness sources | Windows, TOML headers |
| `tools/developer_mode_check/` | Public build ignores Development and remains in normal mode | See harness sources | Windows, C++20 |
| `tools/settings_migration_test/` | The one-time move to the new lighting defaults | `run.ps1` | MSVC x86 |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-05 | `1f76e83` with the 3.0.0 working tree | `settings_migration_test` | 30 passed; `ApexLightingDefaults::Apply` on 75 real tomls of older versions | MSVC x86 |
| 2026-10-09 | 7ddaa214 (release source) | Production package/backend fixture | 3,869 checks passed | Windows files |
| 2026-10-09 | 7ddaa214 | Public developer-mode fixture | Passed | Win32 public |

## In-game test plan

1. Save, apply and import a TOML preset. Expected: only selected categories change.
2. Export a preset with a LUT and import its ZIP on a separate folder setup. Expected: the same LUT is selected, with no manual extraction.
3. Export over an existing final filename and cancel its overwrite confirmation. Expected: the destination stays intact.
4. Assign a shortcut to a personal and a built-in preset, then press each with the menu closed. Expected: the preset applies, a note confirms it and Undo restores the previous settings.
5. Try a combination already used by another preset or by the menu, and a bare letter such as P. Expected: both are refused and the previous keys stay.
6. Type the combination in an Apex text field and in the cheat console. Expected: no preset is applied.

## Confirmed in game

The test-build gameplay check was completed before release preparation. No detailed scenario-by-scenario confirmation was recorded.

## Open checks

- Native dialog overwrite confirmation and ZIP transfer on a separate computer.
- Extended preset switching and all language layouts.
- Preset shortcuts (2.12.2): steps 4 to 6 in game.
- The look shared by the three built-in presets (3.0.0), on a fresh install: not seen in game.

Release provenance: the combined code was independently reviewed at `7ddaa214a6678cdbc0c9554082c779ffc485fa13`. Subsequent documentation-only edits do not change these code paths. The published release tag records the final source commit.

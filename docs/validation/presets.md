# Presets: validation

[Presets](../features/presets.md) must preserve existing TOML settings, reject malformed packages and share a selected LUT without overwriting an unrelated file.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/profile_package_test/` | Package parsing, CRC, paths, collisions, rollback and production import/export backend | See harness sources | Windows, TOML headers |
| `tools/developer_mode_check/` | Public build ignores Development and remains in normal mode | See harness sources | Windows, C++20 |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-09 | 7ddaa214 (release source) | Production package/backend fixture | 3,869 checks passed | Windows files |
| 2026-10-09 | 7ddaa214 | Public developer-mode fixture | Passed | Win32 public |

## In-game test plan

1. Save, apply and import a TOML preset. Expected: only selected categories change.
2. Export a preset with a LUT and import its ZIP on a separate folder setup. Expected: the same LUT is selected, with no manual extraction.
3. Export over an existing final filename and cancel its overwrite confirmation. Expected: the destination stays intact.

## Confirmed in game

The maintainer reported completion of the test-build gameplay check before release preparation. No detailed scenario-by-scenario confirmation was supplied.

## Open checks

- Native dialog overwrite confirmation and ZIP transfer on a separate computer.
- Extended preset switching and all language layouts.

Release provenance: the combined code was independently reviewed at `7ddaa214a6678cdbc0c9554082c779ffc485fa13`. Subsequent documentation-only edits do not change these code paths. The published release tag records the final source commit.

# Save Name Fix: validation

The feature is described in [features/save-name-fix.md](../features/save-name-fix.md). It must set the save's name only
for a `.sims3` opened with `-worldFile`, and change nothing when the bytes are not the ones it knows.

## Automated tests

```powershell
powershell -File tools/save_name_fix_test/run.ps1 [-Dll <Sims3GameplaySystems.dll> [-Patched <file>]]
```

It checks the offsets and branches of the patch, that one match is found and two are refused, and that tokens that
disagree are refused. With `-Dll` (taken out of `gameplay.package`) both patterns must match once; `-Patched` writes the
completed copy, to be decompiled.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-25 | branch `release/4.1.1` | `tools/save_name_fix_test -Dll -Patched` on Steam 1.67.2 | All passed; method at 0x2915FC, tokens at 0x2932E6. Decompiled, the `.sims3` branch ends in `Responder.Instance.OptionsModel.SaveName = text` and the other branches are as before | None |
| 2026-11-25 | branch `release/4.1.1` | The game started with `-worldFile:"ApexTest-Night.sims3"`, its memory read from outside 90 s later | With the fix an `OptionsModel` object holds the save's name; without it none does. The save's files were not written | DXVK |

## In-game test plan

1. Start a save from the launcher's Saves or Home. **Expected:** Save As shows in the options menu at once.
2. Save. **Expected:** no question for a name; the same save is written.
3. Save As with a new name. **Expected:** a second save; the first stays.
4. Go to the main menu and start a new game. **Expected:** Save asks for a name, as always.

## Confirmed in game

- None yet beyond the memory check above.

## Open checks

- The test plan above, on Steam and on the EA app build; a start from the restart host.

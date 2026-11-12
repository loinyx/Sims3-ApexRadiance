# Bone Index Guard: validation

The feature is described in [features/bone-index-guard.md](../features/bone-index-guard.md). It must return the
game's answer for every valid bone index and never read past a part's bone list.

## Automated tests

`tools/bone_index_guard_test` (x86):

```powershell
powershell -File tools/bone_index_guard_test/run.ps1 [-Exe <TS3W.exe>]
```

It calls Apex's getter the way the game does (thiscall, one argument): a valid index gives its entry, an index past
the list the first entry, the crash's case (109 of 1) the only entry, an empty list 0, and the stack stays balanced.
With `-Exe` it checks that the pattern matches once.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-12 | branch `fix/bone-index-guard` | `tools/bone_index_guard_test -Exe` on Steam 1.67.2 TS3W.exe | All checks pass; one match at 0x005C6DF0 | None |
| 2026-11-12 | before the fix | A crash report and its minidump | `TS3W.exe+0x1C6E07` reading `0x7F9501A4` (free); index 0x6D, the part's list 1 entry at `0x7F94FFF0`, the end of a 16 MB region; main thread, caller `0x005D1631` | None |

## In-game test plan

1. Start the game with the fix. **Expected log:** `[BoneIndexGuard] On at 0x005C6E00: ...`.
2. Play with the household and lots that crashed, opening Create a Sim and loading Sims in outfits. **Expected:** no
   crash at `TS3W.exe+0x1C6E07`; a `[BoneIndexGuard] A Sim part asked for bone ...` line when the broken part is built.

## Confirmed in game

None yet.

## Open checks

- The log line in a real session, and the EA app 1.69 build (pattern not checked on it).

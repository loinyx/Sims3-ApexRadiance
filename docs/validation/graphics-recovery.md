# Graphics recovery: validation

[Graphics recovery](../features/graphics-recovery.md) must preserve settings, keep resource teardown on the graphics thread and leave no stale backbuffer reference or reentry flag after a failed effect entry.

## Automated tests

Production AO/Depth saved-state code was extracted into a separate Win32 HAL test host. Every state-capture getter was fault-injected with out-of-memory and device-lost failures; the host also exercised scope restoration and 1,000 reset cycles. These fixtures exclude full effect passes and game hooks. Controlled shader-replacement fixtures exercise the production verifier, including changed reported byte counts.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-09 | d8ce878 plus release integration | State/failure/reset fixture | 124,883 checks, zero failures on each backend | Native D3D9 and explicit DXVK 3.1.1 |
| 2026-10-09 | Same source | Actual RunBlur entry fixture | Three cases passed: failed capture, next successful frame, failed copy | Mock entry dependencies |
| 2026-10-09 | Same source | Production shader verifier | 14 checks passed | Controlled replacements |

After warm-up, handles remained stable at 402 on native and 405 on DXVK. Private bytes increased by 61,440 and 3,047,424 respectively; this short host does not prove absence of leaks in full gameplay. Final device references reached zero.

## In-game test plan

1. Restart with effects enabled. Expected: settings remain and effects recreate their resources.
2. Repeat after changing resolution. Expected: no stuck pending state or inactive Depth Blur.
3. Close and reopen the game. Expected: normal startup and saved settings.

## Confirmed in game

The maintainer reported that the test build had been tested in game. This does not establish separate coverage of each scenario above.

## Open checks

- Long gameplay, all allocation failures, real device-loss transitions and Proton/Linux.
- A separate generated lifetime DLL fixture was refused at load with error 5. The reason remains unknown; no Windows security setting was changed. Module retention follows the Windows process-lifetime pin contract, but that worker/unload fixture did not run.

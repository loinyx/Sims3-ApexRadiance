# Restart host: validation

The host must offer a restart only after a low world save followed by a normal quit, start the same executable with
the same command line plus one `-worldFile` switch for the newest save, and leave the game unchanged while it is off.
The feature is described in [features/performance/restart-host.md](../features/performance/restart-host.md).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `installer/ApexSetup.Tests` | Setup's *Restart host* component: install and removal, the `[ui] restart_host` edit (new file, missing table, existing key, other lines and line endings kept), undo through a restore point, removal with Apex, the off copy older Setups left, and the wait while Apex has older settings to copy | `dotnet run --project installer\ApexSetup.Tests` | .NET 8 SDK |
| `tools/restart_host_test/` | The offer decision (low save, exit code, missing executable), the relaunch command line (switch added, an existing quoted or unquoted `-worldFile` replaced, bad names and overflow refused), the channel's name and checks, the save pick (newest file wins, `.backup` ignored) | `run.ps1` | none |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-12 | branch `feat/ts3wrap-restart-host` | `tools/restart_host_test` | 15 checks pass | none |
| 2026-11-12 | branch `feat/ts3wrap-restart-host` | `tools/development/build.ps1 -Flavor Public` | `ApexRadiance.asi` and `ts3wrap.exe` built | none |
| 2026-11-14 | branch `release/4.0.0`, uncommitted | `installer/ApexSetup.Tests` | all pass, the *Restart host* checks included | none |
| 2026-11-14 | branch `release/4.0.0`, uncommitted | `tools/restart_host_test`; `ts3wrap.vcxproj` Release x64; `restart_host.cpp` compiled alone (x86, `/W4`) | checks pass; built | none |

## In-game test plan

1. `restart_host = true`, `ts3wrap.exe` next to the ASI, start the game. **Expected:** `[RestartHost] ts3wrap.exe started` in the log, `ts3wrap.exe` in Task Manager.
2. Save with more than 320 MB of largest free block, quit. **Expected:** no question, the host ends.
3. Save with less (or lower `kLowBytes` in a test build), quit normally. **Expected:** the question names that save; Yes starts the game and loads it without the main menu.
4. Repeat 3 from the relaunched game. **Expected:** one `-worldFile` switch on the new command line.
5. Start from Steam (`%command%` and the library button). **Expected:** the relaunch loads the save and Steam shows the game as running.
6. Under the RTX Remix bridge. **Expected:** the old bridge server ends and the relaunch renders.
7. After Yes. **Expected:** the new game window takes the focus and the keyboard.
8. `restart_host = false`. **Expected:** no host process, no `[RestartHost]` line.
9. Under DXVK, with a `dxvk.conf` in `Game\Bin` that sets something visible (`dxgi.maxFrameRate`), started with the
   working folder `Game\Bin`. **Expected:** the relaunch has the same frame cap.
10. Setup: *Restart host* on, then off, then *Restore* of each point. **Expected:** `ts3wrap.exe` and the key follow
    the switch; a restore puts back only the key and keeps settings changed in game since.

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan, on Steam 1.67.2 and EA app 1.69.
- The relaunch under each renderer Setup installs (the game's own Direct3D 9, DXVK's latest official release, the RTX
  Remix bridge): checked only in the code (working folder, environment, `d3d9.dll` from the exe's folder); not run.
- Whether the game changes its working folder between the ASI's load and the end of Apex's start-up.
- `-worldFile` loading a save with non-ASCII characters in its name.
- The "Mod Scripts Found" dialog or other start dialogs before the save loads.

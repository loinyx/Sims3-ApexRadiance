# Restart host: validation

The host must offer a restart only after a low world save followed by a normal quit, start the same executable with
the same command line plus one `-worldFile` switch for the newest save, and leave the game unchanged while it is off.
The feature is described in [features/performance/restart-host.md](../features/performance/restart-host.md).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/restart_host_test/` | The offer decision (low save, exit code, missing executable), the relaunch command line (switch added, an existing quoted or unquoted `-worldFile` replaced, bad names and overflow refused), the channel's name and checks, the save pick (newest file wins, `.backup` ignored) | `run.ps1` | none |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-12 | branch `feat/ts3wrap-restart-host` | `tools/restart_host_test` | 15 checks pass | none |
| 2026-11-12 | branch `feat/ts3wrap-restart-host` | `tools/development/build.ps1 -Flavor Public` | `ApexRadiance.asi` and `ts3wrap.exe` built | none |

## In-game test plan

1. `restart_host = true`, `ts3wrap.exe` next to the ASI, start the game. **Expected:** `[RestartHost] ts3wrap.exe started` in the log, `ts3wrap.exe` in Task Manager.
2. Save with more than 320 MB of largest free block, quit. **Expected:** no question, the host ends.
3. Save with less (or lower `kLowBytes` in a test build), quit normally. **Expected:** the question names that save; Yes starts the game and loads it without the main menu.
4. Repeat 3 from the relaunched game. **Expected:** one `-worldFile` switch on the new command line.
5. Start from Steam (`%command%` and the library button). **Expected:** the relaunch loads the save and Steam shows the game as running.
6. Under the RTX Remix bridge. **Expected:** the old bridge server ends and the relaunch renders.
7. After Yes. **Expected:** the new game window takes the focus and the keyboard.
8. `restart_host = false`. **Expected:** no host process, no `[RestartHost]` line.

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan, on Steam 1.67.2 and EA app 1.69.
- `-worldFile` loading a save with non-ASCII characters in its name.
- The "Mod Scripts Found" dialog or other start dialogs before the save loads.

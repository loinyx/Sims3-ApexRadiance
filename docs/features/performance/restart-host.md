# Restart host

When a world save leaves the game with little free address space, quitting the game normally brings up a question:
start The Sims 3 again with that save? Yes starts the game again and loads that save without going through the main
menu. The new process starts with a fresh 32-bit address space. The feature is off by default and has no menu option yet.

## Status

| | |
|---|---|
| Availability | Experimental: `ts3wrap.exe` is in the 3.1.3 release and installed with Apex Radiance by Setup; off by default, no menu option |
| Default | Off |
| Menu | None |
| Configuration | `[ui] restart_host` in `ApexRadiance.toml`, and `ts3wrap.exe` next to `ApexRadiance.asi` |
| Source | [`features/restart_host.cpp`](../../../features/restart_host.cpp), [`native/ts3wrap/ts3wrap.cpp`](../../../native/ts3wrap/ts3wrap.cpp), [`native/ts3wrap/restart_channel.h`](../../../native/ts3wrap/restart_channel.h) |

## The problem

`TS3W.exe` is a 32-bit process. In a long session its address space fragments until a world save cannot find one large
free block ("Error 12", [room-to-save.md](room-to-save.md#the-problem)). Room to save keeps that block free for a
while. Freeing the memory inside the process gives little back, while a new process starts with all of it.

## How Apex Radiance solves it

A small 64-bit host process waits beside the game and starts it again into the last save, only when the player agrees.
The design is adapted from Dalexlol's wrapper in sims3-modernization (PRs #360 and #363).

1. At start, with `restart_host = true`, Apex creates the shared block `Local\ApexRestartHost-<pid>`. It holds the
   game's executable, its command line and the `Saves` folder. Apex then starts `ts3wrap.exe <pid>` from the game's
   folder, so the host inherits the game's start folder and environment.
2. After every successful world save, Room to save's save hook records whether the largest free block (with Room to
   save's reserve held) is under 320 MB, Room to save's own low line.
3. When the game ends with a normal exit code and the last save was low, the host picks the `.sims3` save folder with
   the newest file and asks the player.
4. On Yes, it starts the same executable with the same command line plus `-worldFile:"<save>.sims3"`. A
   `-worldFile` switch already on the line is replaced, so a relaunched game can be relaunched again. The host then
   ends, and the new game starts its own host.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| None | `[ui] restart_host` | bool | `false` | | Starts `ts3wrap.exe` with the game; needs the exe next to `ApexRadiance.asi` |

## Compatibility and interactions

- Needs Room to save running. Its world-save hook gives the signal. With Room to save off, the host never offers a
  restart.
- The host never closes, kills or hosts the game window. A game the player quits without a low save, or a crash, ends
  the host without a question.
- sims3-modernization's own wrapper (`[Wrapper]` in `ts3native.ini`) is a separate program. Use only one of the two.

## Limitations

- The question comes only after the player quits the game. The game does not quit by itself.
- Nothing beyond the save is restored: game speed, the selected Sim and the camera start as the save loads them.
  sims3-modernization restores them with its script mod (`Restart.cs`), which Apex does not have.
- The relaunch is started directly, not through Steam or the EA app. Starting the game from Steam, running under the
  RTX Remix bridge, and window focus after the relaunch are *Unverified* ([validation](../../validation/performance-restart-host.md#open-checks)).
- The question is in English only.

## Technical reference

- Shared block: `RestartChannel` (fixed-width fields, one layout for the 32-bit game and the 64-bit host,
  `static_assert`). The host keeps it mapped past the game's end and checks its magic, version and mapped size.
- Normal exit: exit code 0 (a crash, or Task Manager's End task with code 1, brings no question).
- Save pick: the newest last-write time of a file inside each `*.sims3` folder, because a folder's own time misses
  files rewritten in place. `.backup` folders are ignored.
- Build: `native/ts3wrap/ts3wrap.vcxproj` (x64, static CRT) is part of `ApexRadiance.sln`; the solution's `Release|x86`
  builds it into the same output folder as the ASI.

## Rejected approaches

- The game window as a child of the host window (sims3-modernization step 0): eight `user32` import hooks at Steam
  1.67.2 addresses, with focus, input, DPI and borderless handling left untested. It is not needed for a restart.
- Quitting the game automatically after the low save: needs a script-side quit and state capture (`Restart.cs`).
  Apex has no script mod.

## See also

- [Validation](../../validation/performance-restart-host.md)
- [Room to save](room-to-save.md)

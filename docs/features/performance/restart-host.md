# Restart host

When a world save leaves the game with little free address space, quitting the game normally brings up a question:
start The Sims 3 again with that save? Yes starts the game again and loads that save without going through the main
menu. The new process starts with a fresh 32-bit address space. The feature is off by default and has no in-game menu
option; Setup turns it on with its *Restart host (Experimental)* component.

## Status

| | |
|---|---|
| Availability | Experimental: `ts3wrap.exe` is in the 3.1.3 release; off by default, no in-game menu option. Setup's *Restart host* component (installs `ts3wrap.exe` and sets the key; Setup 1.1.4 and earlier copied the exe with Apex Radiance and left it off) and the relaunch in the game's start folder: Unreleased, planned for 4.0.0 (Setup 1.1.5) |
| Default | Off |
| Menu | None in game; Setup > Components > *Restart host* (Experimental) |
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
   game's executable, its command line and the `Saves` folder. Apex then starts `ts3wrap.exe <pid>` in the game's start
   folder (the working folder read when the ASI loads, before the game runs) with the game's environment, so the host
   and the relaunch it makes inherit both.
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
| None (Setup: *Restart host*) | `[ui] restart_host` | bool | `false` | | Starts `ts3wrap.exe` with the game; needs the exe next to `ApexRadiance.asi` |

## Compatibility and interactions

- Needs Room to save running. Its world-save hook gives the signal. With Room to save off, the host never offers a
  restart.
- The host never closes, kills or hosts the game window. A game the player quits without a low save, or a crash, ends
  the host without a question.
- sims3-modernization's own wrapper (`[Wrapper]` in `ts3native.ini`) is a separate program. Use only one of the two.
- **Renderers Setup installs.** Setup offers three: the game's own Direct3D 9, DXVK (the 32-bit `d3d9.dll` of the
  latest official `doitsujin/dxvk` release, one build) and the RTX Remix bridge (its client `d3d9.dll` in `Game\Bin`,
  its 64-bit server `.trex\NvRemixBridge.exe` with DXVK, `bridge.conf` in `Game\Bin` and `.trex`, `dxvk.conf` in
  `Game\Bin`). What a relaunch must keep the same as a normal start, checked in the code:
  - Working folder: DXVK reads `dxvk.conf` from it unless `DXVK_CONFIG_FILE` is set, and the bridge server reads
    `dxvk.conf` from its working folder, `Game\Bin` ([history](../../history/rtx-remix-bridge.md)). The relaunch uses the
    game's start folder, read when the ASI loads. Before, the host took the folder of the moment Apex started it, at the
    end of Apex's start-up; whether the game changes its working folder before that is unverified.
  - Environment: inherited from the game (`DXVK_*`, `DXVK_CONFIG_FILE`, `DXVK_LOG_PATH`, and the variables Steam
    sets for a game it starts). Variables the game process itself set before Apex started the host are inherited too.
  - `d3d9.dll`: the same executable path is started, and Windows looks for DLLs in the exe's own folder first, so the
    same DXVK or bridge client loads, whatever the working folder.
  - Bridge: the new client starts its own server; `bridge.conf` and `.trex` are found as at a normal start.
  - DXVK's state cache is gone since DXVK 2.0; its logs follow `DXVK_LOG_PATH` (inherited) or its default.
  - Steam and the EA app: the relaunch starts the exe directly, not through the launcher, with the launcher's
    variables inherited from the first start.
  Unverified: an actual relaunch under DXVK, under the bridge (the old server ending before the new one starts), from
  Steam and from the EA app, until tested in game ([validation](../../validation/performance-restart-host.md#open-checks)).

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

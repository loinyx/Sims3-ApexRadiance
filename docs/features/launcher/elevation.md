# Apex Launcher: elevation

The launcher runs as the signed-in player, not as administrator. A change to a game folder the player cannot write to
(usually one under `Program Files`) or to a graphics driver setting asks Windows for permission once, for that change;
a game folder the player can write to, such as a Steam library on another drive, needs no prompt at all. Play never
asks for permission, and the game always starts as the player.

## Status

| | |
|---|---|
| Availability | Released in 4.1.0 (Apex Radiance Setup ran as administrator throughout) |
| Default | Always on |
| Menu | None; the prompt appears with *Administrator permission needed* and *Continue* |
| Configuration | `ops\choices-<id>.txt` in the data folder (a plan, deleted once read) |
| Source | [`Elevation.cs`](../../../installer/ApexSetup.Core/Elevation.cs), [`App.xaml.cs`](../../../installer/ApexSetup/App.xaml.cs) |

## The problem

`Game\Bin` is usually under `Program Files`, which only an administrator can change. A program that runs as
administrator all the time also starts the game as administrator, writes files the player then cannot change, and reads
the administrator's folders instead of the player's.

## How Apex Radiance solves it

1. **Can it write here?** `Elevation.CanWrite` creates and deletes a test file in `Game\Bin` and in the backup folder.
   When both succeed, the change runs in the launcher itself.
2. **A plan.** Otherwise the launcher asks first (*Administrator permission needed*, *Continue*), writes the player's
   choices as `key=value` lines to `ops\choices-<id>.txt` in the data folder and starts its own exe with `runas`:
   `--maintain <Game\Bin> --open <screen> [--choices <file>]`.
3. **The administrator copy** reads and deletes the plan, makes the change (with its restore point), and exits. Its
   window title says *Apex Launcher (administrator)*. The player's window hides while it runs, waits for it to exit, and
   reads the game again.
4. **Other plans.** Game settings (`GameSettingsPlan`), worlds (`WorldsPlan`) and the uninstall's game-folder part
   (`--maintain * --open uninstall` or `uninstall-keep`) run without a window and report back through their exit code.
   Turning NVIDIA Smooth Motion or AMD Fluid Motion Frames on or off always needs administrator rights.
5. **Play** skips safe fixes that would need a prompt, and refuses to start the game from a window that runs elevated
   through UAC (*This window can't start the game*). A launcher started elevated and not installed restarts itself as
   the player through Explorer.

## Settings

None.

## Compatibility and interactions

- The data folder stays the player's even when the administrator copy runs under another administrator account:
  `%LOCALAPPDATA%` is read from the player's shell folder, or from `APEX_LAUNCHER_LOCALAPPDATA`
  ([data files](data-and-files.md)).
- With UAC off or on the built-in Administrator account, `IsUacElevated` is false: the restart as the player and the
  refusal to start the game do not apply.

## Limitations

- The administrator prompt on a game under `Program Files` and the update started by an elevated Apex Radiance Setup
  1.2.x are not yet verified on a real install ([validation](../../validation/installer.md#open-checks)).
- With UAC off or on the built-in Administrator account, the behaviour comes from reading the code; it has not been run
  on such a PC.

## Technical reference

- `Elevation.IsUacElevated` reads the token's elevation type (Full); `IsElevated` is the plain administrator check.
- `ops\open-<id>.txt` is a different hand-over: mod files given to a second launcher instance are passed to the open
  one ([self-update](self-update.md#single-instance)).

## Rejected approaches

- Running the whole program as administrator (Apex Radiance Setup 1.x, through its manifest): replaced by one prompt
  per change, so Play, the data folder and the player's files stay the player's.

## See also

- [Components, install and update](../installer.md), [Uninstall](uninstall.md)
- [Validation](../../validation/installer.md)

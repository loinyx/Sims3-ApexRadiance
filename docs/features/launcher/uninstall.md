# Apex Launcher: uninstall

*Uninstall Apex Launcher* (Settings, or Windows' *Installed apps*) removes everything the launcher installed in every
game it knows, puts back each file it replaced, turns the mods it switched off on again, and then removes the launcher
itself. Saves, save backups, mods the player put in by hand and `Resource.cfg` stay. The player can keep the launcher's
settings, albums and mod notes for a later install.

## Status

| | |
|---|---|
| Availability | Released in 4.1.0 |
| Default | *Keep my settings, albums and mod notes* ticked |
| Menu | Settings > General > Uninstall Apex Launcher; Windows' *Installed apps* |
| Configuration | None |
| Source | [`Uninstall.cs`](../../../installer/ApexSetup.Core/Uninstall.cs), [`MainWindow.Launcher.cs`](../../../installer/ApexSetup/MainWindow.Launcher.cs) (`UninstallAsync`) |

## The problem

An uninstall that only deletes the program leaves its components in the game, the player's own files in a backup
folder and mods switched off in a folder the game does not read. One that deletes too much takes saves or mods with it.

## How Apex Radiance solves it

1. **Refused** while the game runs. An open launcher (also in the notification area) closes first.
2. **Each game** (`Uninstall.FromGame`), through the [administrator copy](elevation.md) when needed: the game settings
   files are put back (`GameSettings.RestoreBin`), every component is removed and its replaced files restored
   (`Engine.RemoveAll`), frame generation the launcher turned on is turned off, and files of the player's that a fix set
   aside in the backup folder go to the Recycle Bin. The backup folder is deleted unless `setup.json` still lists files.
3. **Documents** (`Uninstall.FromDocuments`), as the player: mods turned off by the launcher are turned on again and
   the Disabled folder removed, `Options.ini` gets its launcher values back (`GameSettings.RestoreDocs`), and, unless
   kept, Apex Radiance's settings folder is cleared except its screenshots, videos and captures.
4. **The PC** (`Uninstall.MachineData`): `%ProgramData%\Apex Radiance Setup` (the frame generation records) is deleted
   only when turning frame generation off did not fail, so a later uninstall can try again.
5. **The launcher** (`Uninstall.Launcher`): shortcuts, the *Installed apps* entry, *Open with* and the `Run` value go.
   With *keep* ticked only `thumbs\` and `ops\` leave the data folder; otherwise all of `%LOCALAPPDATA%\Apex Launcher`
   goes. The exe and its folder are deleted after the window closes; from a chosen install folder, only the launcher's
   own files.
6. **A removal that fails half way** leaves `setup.json` true to what is still installed, so a retry never deletes an
   original that was already put back. *The uninstall did not finish* lists what stays, and the launcher stays
   installed.

## Settings

| Menu label | Key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Keep my settings, albums and mod notes | None (asked each time) | check box | On | | Keeps the data folder (except thumbnails and plans) and Apex Radiance's settings |

## Compatibility and interactions

- Never touched: saves, save backups, the player's own mods, `Mods\Resource.cfg`, and the `.apex-backup` copies
  compatibility fixes made of it.
- Removing a single component, or Apex Radiance alone, is in Components > *Repair or remove*
  ([Components](../installer.md)).

## Limitations

- A full uninstall acts on every game the PC has; it has been run on a test PC with a copied game, not on a real install
  ([validation](../../validation/installer.md#open-checks)).

## Technical reference

- The exe deletes itself through a short `cmd` command that waits for the process to end.

## Rejected approaches

None recorded.

## See also

- [Install, portable copy and self-update](self-update.md), [Data folders and files](data-and-files.md)
- [Validation](../../validation/installer.md)

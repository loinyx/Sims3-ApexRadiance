# Apex Launcher: data folders and files

Everything the launcher keeps for itself is in one folder of the player's, `%LOCALAPPDATA%\Apex Launcher`: settings,
the language, notes on mods, profiles, the gallery's albums, thumbnails, the session history and the logs. What it
installs in a game is recorded in that game's `Game\Bin\Apex Radiance backup\setup.json`, with restore points next to
it. Nothing it keeps for itself goes into the game folder or into Documents.

## Status

| | |
|---|---|
| Availability | `%LOCALAPPDATA%\Apex Launcher`: Released in 4.1.0 (Apex Radiance Setup 1.x used `%LOCALAPPDATA%\Apex Radiance Setup`) |
| Default | Created at first start |
| Menu | Settings > General > Logs (*Open log folder*) |
| Configuration | `APEX_LAUNCHER_LOCALAPPDATA` points a test run at another folder |
| Source | [`Paths.cs`](../../../installer/ApexSetup.Core/Paths.cs), [`LauncherSettings.cs`](../../../installer/ApexSetup.Core/LauncherSettings.cs), [`Engine.cs`](../../../installer/ApexSetup.Core/Engine.cs) (`Manifest`), [`RestorePoints.cs`](../../../installer/ApexSetup.Core/RestorePoints.cs) |

## The problem

A launcher that manages mods, saves and pictures keeps a lot of its own state. It must not mix it with the game's files,
lose it when the game is reinstalled, or read the wrong folder when it runs as administrator.

## How Apex Radiance solves it

`Paths.Data` is `%LOCALAPPDATA%\Apex Launcher`, with `%LOCALAPPDATA%` read from the player's shell folder (it stays the
player's when the administrator copy runs) or from `APEX_LAUNCHER_LOCALAPPDATA`. A damaged JSON file is kept as
`.corrupt` and a new one started.

### `%LOCALAPPDATA%\Apex Launcher`

| File or folder | Holds | Page |
|---|---|---|
| `settings.json` | Launcher settings, ignored Health notices and conflicts, game options waiting for `Options.ini` | [Settings](settings.md#settings) |
| `language.txt` | The chosen language (absent: automatic) | [Languages](languages.md) |
| `games.txt` | The games the launcher knows | [Components](../installer.md) |
| `install-folder.txt` | A chosen install folder (absent: the default) | [Self-update](self-update.md) |
| `ApexLauncher.log` | The launcher's log, errors included | [README](README.md#technical-reference) |
| `sessions.json` | The last 20 game sessions | [Home and Play](home-and-play.md) |
| `gallery.json`, `thumbs\` | Albums, favourites, captions, the Home picture, scrapbook looks; picture thumbnails | [Gallery](gallery.md) |
| `backups.json`, `save-stars.json`, `save-sizes.json` | Automatic backup choices, pictures kept by a clean, save size history | [Saves and backups](saves-and-backups.md) |
| `mod-notes.json`, `mod-pictures\` | Notes, tags, creator, kinds, page address and picture of each mod | [Mods](mods.md) |
| `mod-profiles.json` | Mod profiles | [Mods](mods.md#profiles) |
| `mods-view.txt` | The Mods screen's view, sort and the last Play time | [Mods](mods.md) |
| `hunt.txt` | A running *Find a problem mod* and the mods that were on before it | [Mod tools](mod-tools.md) |
| `conflicts.json`, `hook-table.csv` | The conflict scan's cache; the native hook table | [Load order and conflicts](load-order-and-conflicts.md) |
| `game-keys.bin`, `game-names.bin` | The index of the game's own resource keys and names | [Mods](mods.md) |
| `cache-backup\` | Game caches moved aside, until a normal session | [Mod tools](mod-tools.md#clear-game-caches) |
| `release-notes\` | Release notes read from GitHub | [Settings](settings.md) |
| `logs\` | One log per merge | [Mod tools](mod-tools.md#merge) |
| `ops\` | Plans for the administrator copy, files handed from a second launcher | [Elevation](elevation.md) |

### Other places

| Place | Holds |
|---|---|
| `%LOCALAPPDATA%\Programs\Apex Launcher\ApexLauncher.exe` | The installed launcher (or a chosen folder) |
| `Game\Bin\Apex Radiance backup\setup.json` | What the launcher installed in this game, the versions, the copy of `Options.ini`, the Sims 3 Modernization channel |
| `Game\Bin\Apex Radiance backup\*.bak` | Files a component replaced |
| `Game\Bin\Apex Radiance backup\restore\<time>\point.json` | Restore points, the newest 5 |
| `Game\Bin\Apex Radiance backup\game settings\` | The `Game\Bin` game settings files before the first change |
| `Documents\...\Apex Radiance\game settings\` | `Options.ini` before the first change |
| `Documents\...\Mods\Disabled (Apex Launcher)\` | Mods turned off |
| `Documents\...\Merged (Apex Launcher)\` | The originals of each merge, with `merge.json` |
| `Documents\...\Saves kept (Apex Launcher)\` | Saves kept out of the game |
| `Documents\...\Saves backups (Apex Launcher)\` | Save backups (or a chosen folder) |
| `Documents\...\Save companions (Apex Launcher)\` | Mods' declarations of the data they keep for each save ([Saves and backups](saves-and-backups.md#mods-data-beside-a-save)) |
| `%ProgramData%\Apex Radiance Setup\` | Records that the launcher turned NVIDIA Smooth Motion or AMD Fluid Motion Frames on |
| `%TEMP%\ApexLauncher-update\` | A launcher update while it downloads |

`Documents\...` is the game's own Documents folder, `Documents\Electronic Arts\<game folder>`, in the game's language
([architecture.md](../../architecture.md#53-config-file-apexradiancetoml)).

## Settings

None.

## Compatibility and interactions

- At first start the launcher moves Apex Radiance Setup's data folder here ([self-update](self-update.md)).
- The [uninstall](uninstall.md) deletes this folder, or all but its settings, albums and notes when they are kept.

## Limitations

- The data folder is per Windows user; a second account on the same PC starts with its own settings and notes.

## Technical reference

Environment variables for test runs (they never change a normal start):

| Variable | Effect |
|---|---|
| `APEX_LAUNCHER_LOCALAPPDATA` | Another data folder, and with it another single-instance mutex |
| `APEX_SETUP_DOCUMENTS` | A fake Documents folder |
| `APEX_SETUP_DRY_RUN=1` | Shows what the launcher finds and would change, writes nothing, never starts the game; also keeps the copy portable |
| `APEX_LAUNCHER_PORTABLE=1` | Runs as a portable copy |
| `APEX_LAUNCHER_LANG`, `APEX_LAUNCHER_THEME` | A language; `light` or `dark` |
| `APEX_LAUNCHER_NO_DETECT=1` | No automatic game detection; only games the launcher already knows or is given |
| `APEX_LAUNCHER_FAKE_UPDATE` | A fake newer launcher |
| `APEX_LAUNCHER_AUDIT`, `APEX_LAUNCHER_AUDIT_TOUR` | Layout checks and the tour of screens at several window sizes |
| `APEX_SETUP_FAKE_GPU`, `APEX_SETUP_FAKE_NV`, `APEX_SETUP_FAKE_AMD` | Stand-ins for the Vulkan driver, NVAPI and ADLX ([installer/README.md](../../../installer/README.md#build-and-test)) |
| `APEX_SETUP_PREVIEW_LATEST` | Treats the latest Apex Radiance release as the version given (for screenshots of the update screens) |

## Rejected approaches

None recorded.

## See also

- [Apex Launcher](README.md), [Uninstall](uninstall.md)
- [Validation](../../validation/installer.md)

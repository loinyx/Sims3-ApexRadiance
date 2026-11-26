# Apex Launcher

Apex Launcher (`ApexLauncher.exe`) is an optional Windows program for The Sims 3 with Apex Radiance. It installs,
updates, checks, repairs and removes Apex Radiance and the mods it works with, and it is the place the game is started
from: Play, the game's health, the player's mods, saves and backups, pictures and videos, the game's own settings and
the release notes of every component. Up to Apex Radiance 4.0.x it was a one-shot installer, Apex Radiance Setup
(`ApexRadianceSetup.exe`, 1.x); the same program became the launcher in 4.1.0. Apex Radiance does not depend on it: the
manual install in the [README](../../../README.md#installation) gives the same result.

## Status

| | |
|---|---|
| Availability | Released in 3.1.1 as Apex Radiance Setup; Apex Launcher 2.0: Released in 4.1.0; Apex Launcher 2.3: Released in 4.1.1; Apex Launcher 2.3.1 (Load order page, health card on Home): In development (branch `release/4.1.2`) |
| Default | Optional, a separate release asset (`ApexLauncher.exe`, also attached as `ApexRadianceSetup.exe`) |
| Menu | None: a separate program, not part of the in-game menu |
| Configuration | `%LOCALAPPDATA%\Apex Launcher` ([data folders and files](data-and-files.md)); `Game\Bin\Apex Radiance backup\setup.json` |
| Source | [`installer/ApexSetup/`](../../../installer/ApexSetup/) (WinUI 3 window), [`installer/ApexSetup.Core/`](../../../installer/ApexSetup.Core/) (logic), [`installer/ApexSetup.Tests/`](../../../installer/ApexSetup.Tests/) |

## The problem

A modded Sims 3 is spread over several places and projects: the ASI loader, Apex Radiance and an optional renderer in
`Game\Bin`, packages and `Resource.cfg` under `Documents\Electronic Arts\The Sims 3\Mods`, saves next to them, and
pictures in two or three folders. Each piece has its own release page and update cycle, a wrong or duplicate file stops
a mod from loading, and the game itself offers no view of what is installed, which mod clashes with which, or a safe
copy of a save.

## How Apex Radiance solves it

One window gathers the pieces, reads them where the game reads them, and changes files only through actions that keep
a copy or can be undone. The sidebar holds these screens, in this order:

| Screen | Page |
|---|---|
| Home | [Home and Play](home-and-play.md): Play, the newest saves, the health card, the last session |
| Components | [Components, install and update](../installer.md): Apex Radiance, the renderer, the ASI loader and the mods it works with; restore points; Repair or remove |
| Game settings | [Game settings](game-settings.md): options the game keeps in `GraphicsRules.sgr`, `Sims3.ini` and `Options.ini` |
| Mods | [Mods](mods.md): the list, details, adding files, profiles; [Mod tools](mod-tools.md): Find custom content, Find a problem mod, game caches, Merge; [Load order and conflicts](load-order-and-conflicts.md) |
| Saves | [Saves and backups](saves-and-backups.md): saves, cleaning, backups made by hand or automatically |
| Gallery | [Gallery and scrapbook](gallery.md): pictures and videos from Apex Radiance and the game |
| Health | [Health](health.md): one tile per area, fixes, ignored notices, the stability timeline |
| What's new (footer) | [Settings, What's new and languages](settings.md) |
| Settings (footer) | [Settings, What's new and languages](settings.md); [Uninstall](uninstall.md) |

Pages about the program as a whole:

- [Data folders and files](data-and-files.md): everything the launcher writes, and where.
- [Elevation](elevation.md): the launcher runs as the player and asks for administrator rights once per change.
- [Install, portable copy and self-update](self-update.md): the launcher's own install, its update and the move from
  Apex Radiance Setup.
- [Design system](../../launcher-design.md): colours, themes, distances, rows and dialogs.

A first install (a game without Apex Radiance) shows none of the sidebar: one card in the middle of the window walks
through the game, the components (with the renderer), the game settings, the install and the done screen
([Components, install and update](../installer.md)).

## Settings

Every launcher setting is listed once, on the page of its screen: [Settings](settings.md#settings) (Starting the game,
Updates, Appearance, General) and [Saves and backups](saves-and-backups.md#settings) (automatic backups).

## Compatibility and interactions

- Windows 10 version 1809 or later, 64-bit (`TargetPlatformMinVersion` 10.0.17763.0, `win-x64`).
- Steam 1.67.2 (`TS3W.exe`), the disc build 1.67.2, EA app 1.69.47 and Origin 1.69.43 (`TS3.exe`) are recognised by the
  exe's PE timestamp (`Games.Detect`); an unknown build with `TS3.exe` is taken as EA, otherwise as Steam.
- The launcher refuses to change files while `TS3W.exe`, `TS3.exe`, `Sims3Launcher.exe`, `Sims3LauncherW.exe`,
  `ts3wrap.exe` or `NvRemixBridge.exe` runs (`Games.Processes`).
- It needs an internet connection to download components and release notes; everything else works offline.

## Limitations

- The *Worlds* screen is in the code (`Worlds.cs`, `WorldsPlan`) but its sidebar entry is hidden
  (`Visibility="Collapsed"` in `MainWindow.xaml`); nothing in the code shows it.
- Behaviour on a real install that is not yet confirmed is listed under *Open checks* on the
  [validation page](../../validation/installer.md#open-checks).

## Technical reference

- WinUI 3 on .NET 8 (`net8.0-windows10.0.19041.0`), published as one self-contained, trimmed x64 exe
  (`PublishSingleFile`, no MSIX, no runtime to install; [ApexSetup.csproj](../../../installer/ApexSetup/ApexSetup.csproj)).
  The assembly name is `ApexLauncher`; the product name stays *Apex Radiance Setup* so that Setups before the launcher
  accept the update ([self-update](self-update.md)).
- The window is one `MainWindow` split into partial classes per screen (`MainWindow.Home.cs`, `MainWindow.Mods.cs`,
  `MainWindow.Saves.cs`, `MainWindow.Gallery.cs`, `MainWindow.Health.cs`, `MainWindow.GameSettings.cs`,
  `MainWindow.Launcher.cs` for Settings, What's new and Play, and so on). Logic that needs no window lives in
  `ApexSetup.Core` and is what the tests call.
- Every write to the game folder or Documents goes through `Fs` in `ApexSetup.Core`, so restore points see it
  (`RestorePoints.cs`).
- Errors are logged to `%LOCALAPPDATA%\Apex Launcher\ApexLauncher.log` (`App.LogPath`).
- Build, publish and tests: [workflow.md](../../workflow.md#apex-launcher). Release assets:
  [release-process.md](../../release-process.md#4-build-and-publish).

## Rejected approaches

- Sims3Pack support (installing `.sims3pack` files the way the game's own launcher does): removed; `.sims3pack` files
  are refused with a pointer to the game's launcher ([Mods](mods.md#adding-files)).

## See also

- [Validation](../../validation/installer.md): tests, results and open checks
- [installer/README.md](../../../installer/README.md): component sources, compatibility fixes and suggestions
- [Design system](../../launcher-design.md)

# Apex Radiance Setup

A Windows program, `ApexRadianceSetup.exe`, that installs Apex Radiance and the mods it works with into The Sims 3,
keeps them up to date, checks the installation and repairs or removes it. The player picks the game and the components;
Setup downloads each file from its author's own release and changes only what differs from that choice. Every change
Setup makes can be undone from a restore point. It is optional: the manual install in the
[README](../../README.md#installation) gives the same result.

## Status

| | |
|---|---|
| Availability | Released in 3.1.1; `ApexRadianceSetup.exe` is a release asset. Setup's own update: Released in 3.1.4 (Setup 1.1.0). Modernization from the newest release with the game's build, a moved EA app game: Released in 3.1.5 (Setup 1.1.1). Installs from the manual-install zip (the release's two files): Released in 3.1.6 (Setup 1.1.2) |
| Default | Optional, a separate download |
| Menu | None: a separate program, not part of the in-game menu |
| Configuration | `Game\Bin\Apex Radiance backup\setup.json` (written by Setup); restore points in `Game\Bin\Apex Radiance backup\restore`; language in `%LOCALAPPDATA%\Apex Radiance Setup\language.txt` |
| Source | [`installer/`](../../installer/) ([installer/README.md](../../installer/README.md)) |

## The problem

A manual install needs several files from different projects placed in `Game\Bin`: the ASI loader, Apex Radiance, an
optional renderer (DXVK or the RTX Remix bridge, whose `bridge.conf` must name the game's exe) and the other mods a
player uses. Each has its own release page and its own update cycle, a wrong or duplicate file stops a mod from loading,
and a file overwritten by hand cannot be put back.

## How Apex Radiance solves it

Setup finds the game, compares what is installed with the player's choice and applies the difference:

1. **Game.** It finds Steam 1.67.2 (`TS3W.exe`) and EA app 1.69.47 (`TS3.exe`) installs, or a folder the player picks.
2. **Components.** Apex Radiance is required. The renderer is DXVK (recommended), the RTX Remix bridge zip of the Apex
   release (offered only when a graphics card has Vulkan 1.3, which its DXVK 2 needs), or the game's own Direct3D 9; Ultimate ASI Loader is added only when no loader is found; other mods that work
   with Apex can be switched on. The full list, with sources and target folders, is in
   [installer/README.md](../../installer/README.md#what-it-does).
3. **Install.** Every file is downloaded at install time from its author's GitHub release (never re-hosted) and checked
   against the release's SHA-256 digest when it has one. A file is copied only when it is missing or older than the
   latest release. Switching an installed component off removes it.
4. **Backups.** Every file Setup replaces is moved to `Game\Bin\Apex Radiance backup` with `.bak` appended, so no
   loader reads it. `setup.json` in that folder records what Setup installed.
5. **Compatibility fixes.** At the end of every install, or with *Fix settings* on the Components step, Setup runs the checks of the game's
   Settings > Compatibility page before the game starts and fixes what it can (a second ASI loader or Apex copy, a
   wrong `client.targetProcess`, a missing `Resource.cfg`); the rest is reported with a link.
6. **Suggestions.** What Apex Radiance recommends in game is listed as *Suggested for Apex Radiance* on the Components
   step and on the game's status, only while `ApexRadiance.asi` is installed: turning off the game's Edge Smoothing
   when it blocks Apex effects that are on, moving a renamed second copy of `ApexRadiance.asi` aside, and the renderer
   and Sims3SettingsSetter Apex recommends. None runs with Install or Repair: each has its own *Apply* (or *Apply all
   suggestions*), and a component suggestion points to Components. Applying one keeps the Components choices not yet
   installed. The full list is in [installer/README.md](../../installer/README.md#suggestions).
7. **Restore points.** Before every change (install, update, repair, removal, a fix or a suggestion) Setup opens a
   restore point: a copy of each file as it was before Setup first touched it, in `Game\Bin` and in Documents, and the
   files and folders it created. Every write of `ApexSetup.Core` goes through one function, so nothing is left out, and
   a file's read-only attribute is kept. An action that changes nothing keeps no point; the newest 5 are kept. The
   *Restore points* card on *Repair or remove* lists them; *Restore* puts the files back and undoes every newer point
   too. When a change fails, its error offers *Undo these changes*.
8. **Check installation.** On a game that has Apex Radiance, the welcome page shows what is installed and whether it is
   up to date, with *Update now* for the components that have a newer release. *Check installation* scans the game
   again and verifies the installed files and the compatibility checks.
   **Setup's own update:** when the latest Apex Radiance release has an `ApexRadianceSetup.exe` of a higher version
   (Apex Radiance Setup, checked against GitHub's SHA-256 digest), the welcome page shows *A new Setup is available*
   with *Update Setup*: the running exe is renamed to `.old` (deleted at the next start), the new one takes its place
   and opens. Setups before 1.1.0 (Apex Radiance 3.1.3 and earlier) do not look for it.
9. **Repair or remove.** *Repair* downloads and copies again everything installed. Each component can be removed on its
   own, putting back the files it replaced. *Uninstall everything* removes Apex Radiance and what Setup installed and
   restores every replaced file; mods installed by hand and the settings in Documents stay.

When the renderer changes the graphics card the game sees, the game resets its graphics options once: Setup says so
and keeps a copy of `Options.ini`, restored when the player goes back to the game's own Direct3D 9.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Language (title bar button) | None (`language.txt`) | choice | Windows' display language | The 21 languages of The Sims 3, as the in-game menu | Setup's language; a text without a translation shows in English |

## Compatibility and interactions

- Setup refuses to change files while `TS3W.exe` or `TS3.exe` runs.
- It needs administrator rights (`Game\Bin` is usually under Program Files) and an internet connection to download.
- Mods published outside GitHub are recognised and listed with a link to their official page, never downloaded. The
  Sims 3 Performance Patch is on Nexus Mods only: Setup opens its page and installs the file when the download
  finishes.
- Apex Radiance itself does not depend on Setup; a manual install and a Setup install are the same files.

## Limitations

- Windows 10 version 1809 or later, 64-bit.
- Without the GitHub API (60 calls an hour per address) Setup reads the release pages instead.
- Restore points cover the files Setup changes; files changed by hand are not tracked. Restoring is final, needs the
  game closed, and only the newest 5 points are kept.
- The Edge Smoothing suggestion reads `Options.ini` `edgesmoothing`, where Apex reads the back buffer: 0 was seen with
  no MSAA, and that every other value means MSAA is unverified in game. It changes the file only when the player clicks
  *Apply*.

## Technical reference

- WinUI 3 on .NET 8, published as one self-contained x64 exe (`PublishSingleFile`, no MSIX, no runtime to install;
  [ApexSetup.csproj](../../installer/ApexSetup/ApexSetup.csproj)). The engine and the compatibility checks are in
  [ApexSetup.Core](../../installer/ApexSetup.Core/) (`Engine.cs`, `Compat.cs`); restore points in `RestorePoints.cs`
  (`Fs` is the one place that writes).
- Each language is a `Strings.<code>.cs` table with the English text as key; `ApexSetup.Tests` checks every table.
- Errors are logged to `%TEMP%\ApexRadianceSetup.log`. `APEX_SETUP_DRY_RUN=1` shows what Setup finds and would change
  while writing nothing.
- Build and tests: [installer/README.md](../../installer/README.md#build-and-test); the release step is in
  [release-process.md](../release-process.md#4-build-and-publish).

## See also

- [installer/README.md](../../installer/README.md): components, compatibility fixes, suggestions, build and test
- [README installation](../../README.md#installation): the manual install
- [RTX Remix bridge](rtx-remix-bridge.md), [Display](display.md)

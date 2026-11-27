# Apex Launcher: install, portable copy and self-update

A downloaded `ApexLauncher.exe` asks once whether to install itself or run without installing. Installed, it lives in
`%LOCALAPPDATA%\Programs\Apex Launcher` (or a folder the player picks), with a Start menu entry, an optional desktop
shortcut, an *Installed apps* entry, *Open with* for `.package` files and an optional start with Windows. When a newer
launcher is published with Apex Radiance, it joins the other updates on Home's update button and replaces itself. An Apex Radiance Setup
1.x updates itself into the launcher and keeps its language, games and settings.

## Status

| | |
|---|---|
| Availability | Setup's own update: Released in 3.1.4 (Setup 1.1.0); installed or portable launcher: Released in 4.1.0; one install screen with folder, desktop shortcut and Start with Windows, moving an installed launcher: Released in 4.1.1 |
| Default | Asked on first start (*Install Apex Launcher* or *Use without installing*) |
| Menu | The install card at first start; Settings > General > *Apex Launcher on this PC* |
| Configuration | `install-folder.txt` (a chosen folder), `ApexLauncher.portable` next to a portable exe, `settings.json` `DesktopShortcut` |
| Source | [`LauncherInstall.cs`](../../../installer/ApexSetup.Core/LauncherInstall.cs), [`SelfUpdate.cs`](../../../installer/ApexSetup.Core/SelfUpdate.cs), [`App.xaml.cs`](../../../installer/ApexSetup/App.xaml.cs), [`MainWindow.Upgrade.cs`](../../../installer/ApexSetup/MainWindow.Upgrade.cs), [`Paths.cs`](../../../installer/ApexSetup.Core/Paths.cs) |

## The problem

A program downloaded to `Downloads` is easy to lose, leaves several copies behind after updates, and cannot start with
Windows or open files from Explorer. Players of Apex Radiance Setup 1.x must reach the launcher without reinstalling
anything.

## How Apex Radiance solves it

1. **Mode** (`LauncherInstall.ModeOf`): *Installed* when the exe is the installed copy; *Portable* when an
   `ApexLauncher.portable` file sits next to it or `APEX_LAUNCHER_PORTABLE=1`; otherwise *not chosen*.
2. **First start** (`FirstStep`). An installed copy registers itself again (shortcuts, entry, *Open with*). A copy not
   chosen, with an installed copy present, copies itself in when it is newer and hands over (the older copy closes
   unless it is in the middle of a change); an older or equal download only opens the installed copy. Otherwise the
   install card asks: *What's new* (only for a former Setup), *Where to keep it* (folder, *Desktop shortcut*, *Start
   with Windows*, *Install* or *Use without installing*), *Ready*. The installed copy can delete the copy left in
   Downloads.
3. **Registration** (`Register`): Start menu `Apex Launcher.lnk`, the desktop shortcut when chosen, the *Installed apps*
   entry `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ApexLauncher` (uninstall command `--uninstall`),
   *Open with* for `.package` (`ApexLauncher.Mod`, offered, never made the default) and, when chosen, the `Run` value
   *Apex Launcher* with `--tray`. A chosen install folder must be local and writable; network drives are refused.
4. **Self-update** (`SelfUpdate.CheckSetupUpdateAsync`). The latest `loinyx/Sims3-ApexRadiance` release is read for
   `ApexLauncher.exe` (or `ApexRadianceSetup.exe`). The download must match GitHub's SHA-256 digest, have a higher file
   version and the product name *Apex Launcher* or *Apex Radiance Setup*. An info bar says *A new version of Apex
   Launcher is available*; when it is ready, it is one more line on Home's update button (*Install N updates*, beside
   Play), installed last since it restarts the launcher; there is no dialog of its own. The running exe
   is renamed `.old` (deleted at the next start) and the new one takes its place. No update runs in a dry run, in the
   administrator copy or during `--uninstall`.
5. **From Apex Radiance Setup.** Setup 1.1.0 and later update to the release asset `ApexRadianceSetup.exe`, which is the
   launcher's exe under that name (its product name stays *Apex Radiance Setup* for that reason). At first start
   `Paths.MigrateOldData` moves `%LOCALAPPDATA%\Apex Radiance Setup` to `Apex Launcher`, or copies the missing files and
   marks the old folder `.copied-to-apex-launcher`; the install card then opens on *What's new*.

### Single instance

One launcher runs at a time (mutex `Local\ApexLauncher`). A second start brings the open window forward and passes any
files it was given (*Open with*) through `ops\open-<id>.txt`. `--replace` asks the open copy to quit; `--handover`
waits up to 20 s for it.

## Settings

| Menu label | Key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Desktop shortcut (install card) | `settings.json` `DesktopShortcut` | switch | On | | A desktop shortcut for the installed copy |
| Start with Windows | Windows' `Run` value | switch | Off | | Opens in the notification area at sign-in; only for an installed copy |
| Install folder | `install-folder.txt` | folder | `%LOCALAPPDATA%\Programs\Apex Launcher` | A local, writable folder | Where the installed copy lives; *Move…* in Settings |

## Compatibility and interactions

- The release ships the same exe twice, as `ApexLauncher.exe` and `ApexRadianceSetup.exe`
  ([release-process.md](../../release-process.md#4-build-and-publish)).
- Setups before 1.1.0 (Apex Radiance 3.1.3 and earlier) do not look for updates.
- `APEX_LAUNCHER_LOCALAPPDATA` gives a test copy its own data folder and its own mutex, so it never talks to the
  player's launcher.

## Limitations

- Not yet verified on a real install: the upgrade from Apex Radiance Setup 1.2.x, and the update an elevated Setup 1.2.x
  starts ([validation](../../validation/installer.md#open-checks)).
- Without the GitHub API (60 calls an hour per address) the launcher reads the release page instead.

## Technical reference

- Downloads go to `%TEMP%\ApexLauncher-update\`, outside the data folder.
- `APEX_LAUNCHER_FAKE_UPDATE` offers a fake newer launcher for a test run.
- Command-line switches: `--uninstall`, `--tray`, `--handover`, `--replace`, `--was-setup`, `--moved-from=<path>`,
  `--maintain`, `--open`, `--choices`, and file paths for *Open with*.

## Rejected approaches

None recorded.

## See also

- [Uninstall](uninstall.md), [Elevation](elevation.md), [Data folders and files](data-and-files.md)
- [Validation](../../validation/installer.md)

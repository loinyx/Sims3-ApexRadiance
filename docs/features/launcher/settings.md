# Apex Launcher: Settings and What's new

Settings holds the launcher's own choices: how Play behaves, the look (colour theme, light or dark, the picture on top
of Home, the window background), the language, the game folder, the logs, *Repair or remove*, where the launcher lives
on the PC, the credits and the launcher's uninstall. *What's new* shows the release notes of Apex Radiance and the
components it works with, read from their GitHub releases.

## Status

| | |
|---|---|
| Availability | Released in 4.1.0; colour themes and *Light or dark*: Released in 4.1.1 |
| Default | See the table |
| Menu | Settings and What's new, at the foot of the sidebar |
| Configuration | `settings.json`, `language.txt`, `gallery.json` `HomePicture`; Windows' `Run` key ([data files](data-and-files.md)) |
| Source | [`MainWindow.Launcher.cs`](../../../installer/ApexSetup/MainWindow.Launcher.cs) (`BuildSettings`, `BuildNews`), [`LauncherSettings.cs`](../../../installer/ApexSetup.Core/LauncherSettings.cs), [`Releases.cs`](../../../installer/ApexSetup.Core/Releases.cs), [`Design/Themes.cs`](../../../installer/ApexSetup/Design/Themes.cs) |

## The problem

A launcher that starts the game, hides itself and applies fixes needs a place where the player decides how much it does
on its own, and the release notes of five projects live on five GitHub pages.

## How Apex Radiance solves it

1. **Settings** is one page in four groups: *Starting the game*, *Updates*, *Appearance* and *General* (table below).
   A change is saved to `settings.json` at once.
2. **Look.** The colour theme colours only what stands out (the main button, switches, the selected place, links);
   surfaces stay neutral ([design system](../../launcher-design.md)). *Light or dark* follows Windows or stays as
   chosen, also while the launcher is open. The theme also colours Apex Radiance's menu in the game: the launcher
   writes its colours to `[ui] theme_*` in `ApexRadiance.toml` when it starts, when the theme changes and before Play
   ([menu palette](../../ui.md)).
3. **General** rows: *Language* (*Change*, see [Languages](languages.md)), *Game folder* (*Change*), *Logs* (*Open log
   folder*), *Repair or remove* (*Open*, [Components](../installer.md)), *Apex Launcher on this PC* (move, install, or
   use without installing; [self-update](self-update.md)), *Credits and licences*, and *Uninstall Apex Launcher*
   ([Uninstall](uninstall.md)).
4. **What's new** (`BuildNews`) always shows Apex Radiance and Sims 3 Modernization, and Ultimate ASI Loader,
   MonoPatcher and DXVK when they are installed. Each release is marked *Installed*, *Latest* or *Newer than yours*, its
   notes are rendered from Markdown with *Open on GitHub*. The list comes from the GitHub API (15 s timeout), then the
   release page (20 s); notes already read are kept in `release-notes\` for offline use.

## Settings

| Menu label | Key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Hide while playing | `settings.json` `HideWhilePlaying` | switch | On | | The launcher goes to the notification area while the game runs |
| Apply safe fixes without asking | `settings.json` `FixWithoutAsking` | switch | Off | | Play fixes the fixable compatibility findings without asking; off, it asks first ([Health](health.md)) |
| Install updates when I press Play | `settings.json` `InstallUpdatesOnPlay` | switch | On | | The ready updates go in before the game starts; off, Play starts the game as it is |
| Theme | `settings.json` `Theme` | choice | Amethyst | Amethyst, Orchid, Aurora, Lagoon, Emerald, Honey, Sunset, Cherry, Sakura, Graphite, Moonlight, or Windows' accent colour (`windows`) | The colour of the main button, switches, links and the selected place |
| Light or dark | `settings.json` `Mode` | choice | Same as Windows (`system`) | Same as Windows, Dark, Light | The launcher's light or dark look |
| Picture at the top of Home | `settings.json` `HomePicture` | choice | A picture from the gallery (`gallery`) | A picture from the gallery, The household's photo (`household`), No picture (`none`) | What shows behind the game's name on Home |
| Gallery picture | `gallery.json` `HomePicture` | buttons | The newest picture | *Choose in the gallery*, *Use the newest* | The gallery picture on Home: the one marked *Use on Home*, else the newest |
| Picture strength | `settings.json` `HomePictureStrength` | slider | 100% | 30 to 100%, step 5 | How strongly the picture shows |
| Window background | `settings.json` `Backdrop` | choice | Mica (Windows default) (`mica`) | Mica (Windows default), Mica, stronger (`micaalt`), Acrylic (`acrylic`), Solid colour (`solid`) | Mica and Acrylic take a tint from the desktop wallpaper; Solid colour takes none |
| Start with Windows | Windows' `HKCU\...\Run` value *Apex Launcher* | switch | Off | | Opens in the notification area at sign-in (`--tray`); only for an installed copy |
| Desktop shortcut (install card) | `settings.json` `DesktopShortcut` | switch | On | | A desktop shortcut for the installed copy |

The automatic backup settings are on [Saves and backups](saves-and-backups.md#settings).

## Compatibility and interactions

- A Windows contrast theme keeps the brushes' written values ([design system](../../launcher-design.md)).
- Without the GitHub API (60 calls an hour per address) the launcher reads the release pages instead.

## Limitations

- Unverified on a real install: the Acrylic and stronger Mica backgrounds
  ([validation](../../validation/installer.md#open-checks)).

## Technical reference

- `settings.json` also holds keys no Settings row shows: `NavCompact`, `WelcomedBack`, `PendingOptions`,
  `IgnoredConflicts`, `IgnoredFindings` ([data files](data-and-files.md)).
- `APEX_LAUNCHER_THEME` (`light` or `dark`) forces the look for a test run.

## Rejected approaches

None recorded.

## See also

- [Languages](languages.md), [Data folders and files](data-and-files.md)
- [Validation](../../validation/installer.md)

# Apex Launcher: Home and Play

Home is the launcher's first screen. A picture on top carries the game's name, the Play button and a health card; under
it stand the newest saves, each with its own Play button, and the newest pictures. Play starts the game at its main
menu, a save's Play starts the game straight into that save, and while the game runs the top shows the playing time
and the memory the game uses. After a crash or a session that ran out of memory, Home offers to open that save again.

## Status

| | |
|---|---|
| Availability | Released in 4.1.0; one row of saves with their own Play: Released in 4.1.1; the health card over the picture: Released in 4.1.2 |
| Default | Shown at start when a game is found |
| Menu | Home |
| Configuration | `settings.json` (`HideWhilePlaying`, `InstallUpdatesOnPlay`, `FixWithoutAsking`, `HomePicture*`), `sessions.json` ([data files](data-and-files.md)) |
| Source | [`MainWindow.Home.cs`](../../../installer/ApexSetup/MainWindow.Home.cs), [`MainWindow.Launcher.cs`](../../../installer/ApexSetup/MainWindow.Launcher.cs) (`PlayAsync`, `StartChecked`, `StartGameAsync`), [`Launch.cs`](../../../installer/ApexSetup.Core/Launch.cs), [`SessionHistory.cs`](../../../installer/ApexSetup.Core/SessionHistory.cs), [`Tray.cs`](../../../installer/ApexSetup/Tray.cs) |

## The problem

The game's own launcher only starts the game. A player who wants to know whether the mods load, which save was played
last, or why the last session ended has to look in several folders and logs; a save is opened only through the game's
menus after the main menu has loaded.

## How Apex Radiance solves it

1. **The top.** The picture chosen in [Settings](settings.md#settings) (a gallery picture, the household's photo of the
   newest save, or none) at its own width-to-height, so nothing of it is cut, the game's name, Play, an *Install N
   updates* button while updates are ready (`UpdateOnlyAsync`: the install card, then the done screen, without starting
   the game; a failure says "The update wasn't installed" and Back returns Home) and a *More options* menu (*Play without
   updating*, *All saves*). Over the
   picture, a card of dark glass holds the health crystal, the state (*Ready to play*, *Needs your attention*,
   *Apex Radiance can't start*), the first thing to look at, and a way into [Health](health.md).
2. **Banners.** *Apex Radiance can't start* when Health is Blocked (Play becomes *Play anyway*, "The game starts without
   Apex Radiance"); *No internet* with *Try again*; *The game closed on its own* or *Ran out of memory* when the last
   session ended so, with the time played, the memory peak and *Open this save again*.
3. **Your saves.** The 4 newest saves, each with its household picture, when it was saved and a round Play button; a
   line counts the shown saves without a backup and offers *Back up now* / *Back up all*
   ([Saves and backups](saves-and-backups.md)).
4. **Your pictures.** The 6 newest gallery pictures; each has a pin for *Use on Home* ([Gallery](gallery.md)).
5. **Play** (`PlayAsync`):
   - The game already running: "The Sims 3 is already running".
   - Updates waiting and *Install updates when I press Play* on: a dialog *N updates install first* with *Update and
     play*, *Play without updating* and *Cancel*. The updates run in the install card, then the game starts.
   - Health runs its before-play checks only (installed files and compatibility, `Health.Gather(beforePlay: true)`).
     With a game folder the player can write to, the fixable compatibility findings are offered first (*Fix the game's
     files first?*: *Fix and play*, *Play without fixing*, *Cancel*), or fixed without the question when *Apply safe
     fixes without asking* is on. Play never shows an administrator prompt.
   - Still Blocked: *One thing before you play* with *Open Health*, *Play anyway* and *Cancel*.
   - Automatic backups set to *Before playing* run ([Saves and backups](saves-and-backups.md#automatic-backups)), Game
     settings choices waiting for the game's `Options.ini` are applied, and the game starts.
6. **Start** (`Launch.Start`). The launcher starts the game's exe (`TS3W.exe` or `TS3.exe`, `Game.ExePath`) itself, as
   the player, with `Game\Bin` as the working folder. A save's Play adds `-worldFile:"<name>.sims3"`. For the Steam
   edition it sets `SteamAppId=47890`. When the exe asks for administrator rights (Win32 error 740), it starts through
   the shell instead.
   The EA app edition goes through the EA app instead: its `TS3.exe` started by anyone else asks the EA app to start the
   game, and the EA app starts its own target, `Sims3Launcher.exe -test` (`__Installer\installerdata.xml`), so the Sims 3
   launcher would open. Play opens `origin2://game/launch?offerIds=<id>&cmdParams=<params>` with the first content id
   of `installerdata.xml` (`sims3_dd`) and `-editInGameMode:true0`: with it the Sims 3 launcher starts `TS3.exe` at once,
   passing every argument on, without its window. The launcher only checks that the value starts with `true`; the game
   turns its Edit Town mode on for `true` alone (a save opened that way had no Sims and no lots to pick), not for `true0`. A save goes first as `-worldFile:<name>.sims3"` (the EA app adds the
   opening quote itself and lowercases the switch). The launcher then waits up to 2 minutes for `TS3.exe`.
7. **The session** (`Session` in `Launch.cs`). Every 5 s the launcher reads the game's private memory and keeps the
   peak; it follows a restart of the game (by the restart host or within about 20 s). The end is classed as a crash
   (an exit code of `0xC0000000` or more, or a crash file written since the start: the game's `xcpt *.txt`,
   `ApexRadiance_Crash.txt` or `.dmp`) or as low memory (a peak of 3,400 MB or more, `Session.LowMemoryBytes`). A crash
   always offers *Open this save again*; low memory offers it only while the restart host is off. Sessions are kept in
   `sessions.json` (the newest 20) and feed Health's Stability tile.
8. **Hide while playing.** With it on, the window hides while the game runs and a notification-area icon shows
   "The Sims 3 · time · MB"; the window returns when the game closes. The icon's menu has *Open Apex Launcher* and
   *Quit*. `--tray` (used by *Start with Windows*) starts the launcher in the notification area only.
9. **After the game closes.** Automatic backups set to *When the game closes* run 10 s later, and the Mods screen's *New
   since last play* place is reset.

When no game is found, Home asks for the game folder (*Choose the game*).

## Settings

The settings that change Home and Play are on the [Settings](settings.md#settings) page: *Hide while playing*, *Apply
safe fixes without asking*, *Install updates when I press Play*, *Picture at the top of Home*, *Gallery picture*,
*Picture strength*.

## Compatibility and interactions

- A save opened with `-worldFile` keeps its name in the game through Apex Radiance's
  [Save name fix](../save-name-fix.md).
- The [restart host](../performance/restart-host.md) restarts the game in a new process; the session follows it while
  `ts3wrap` runs.
- The launcher refuses to start the game from a window running with administrator rights through UAC ("This window
  can't start the game"), so the game never runs elevated by accident ([Elevation](elevation.md)).
- With `APEX_SETUP_DRY_RUN=1` the game is never started.

## Limitations

- The crash and low-memory classification reads exit codes, crash files and the memory peak; a freeze the player ends
  by hand counts as a crash only when the exit code says so.
- The memory meter shows the game's private memory against 3.3 GB, a rule of thumb for the 32-bit game, not the exact
  point where it fails.
- Unverified on a real install: the session with the EA app edition, a save opened through the EA app, and the
  notification-area icon. A save opened from Home and the crash state on Home are listed as open checks on the
  [validation page](../../validation/installer.md#open-checks).

## Technical reference

- `Session.ShouldOffer`, `Session.Classify` and `SessionHistory` are in `ApexSetup.Core`; `CrashTimeline` and
  `CrashEvidence` match the sessions to Windows events and logs for [Health](health.md).
- The comment at the top of `Tray.cs` names *Play* in the icon's menu; the menu built by `TrayShow` has only *Open Apex
  Launcher* and *Quit*.

## Rejected approaches

None recorded.

## See also

- [Apex Launcher](README.md)
- [Validation](../../validation/installer.md)

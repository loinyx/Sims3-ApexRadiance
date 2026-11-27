# Apex Launcher: Health

Health tells whether the game is ready to play with Apex Radiance and what needs a look. A crystal in the state's
colour (green to red) heads the screen, with one tile per area: Apex Radiance, Mods, Mod loaders, Stability, Graphics,
Saves, Game settings, and Space and caches. Each tile opens its own page with the findings, a fix where one is safe, and
*Ignore* for a notice the player does not want to see. *Blocked* means only that Apex Radiance itself cannot load; the
game still starts without it.

## Status

| | |
|---|---|
| Availability | Released in 4.1.0; one page per check and ignored notices: Released in 4.1.1 |
| Default | Checked at start and before Play; safe fixes offered before Play (*Apply safe fixes without asking*, off) |
| Menu | Health (a badge on the sidebar entry when something needs a look); the health card on [Home](home-and-play.md) |
| Configuration | `settings.json` `IgnoredFindings`, `IgnoredConflicts`, `FixWithoutAsking`; `sessions.json` |
| Source | [`MainWindow.Health.cs`](../../../installer/ApexSetup/MainWindow.Health.cs), [`Crystal.cs`](../../../installer/ApexSetup/Crystal.cs), [`Health.cs`](../../../installer/ApexSetup.Core/Health.cs), [`Compat.cs`](../../../installer/ApexSetup.Core/Compat.cs), [`Verify.cs`](../../../installer/ApexSetup.Core/Verify.cs), [`ScriptErrors.cs`](../../../installer/ApexSetup.Core/ScriptErrors.cs), [`CrashTimeline.cs`](../../../installer/ApexSetup.Core/CrashTimeline.cs), [`CrashEvidence.cs`](../../../installer/ApexSetup.Core/CrashEvidence.cs), [`PlaceChecks.cs`](../../../installer/ApexSetup.Core/PlaceChecks.cs), [`GameCaches.cs`](../../../installer/ApexSetup.Core/GameCaches.cs) |

## The problem

When a modded game misbehaves, the cause can be anywhere: a second ASI loader, a renamed copy of a mod, a renderer
configured for the wrong exe, two mods replacing the same data, a save growing out of control, a full disk or a graphics
driver reset. The game reports none of it; its crash logs name addresses, not mods.

## How Apex Radiance solves it

1. **Gather** (`Health.Gather`). Each analyser runs on its own, and one that fails is logged without stopping the
   others: installed files (`Verify.Files`), compatibility checks (`Compat.Check`), mod checks, conflicts
   ([Load order and conflicts](load-order-and-conflicts.md)), lost or orphaned merges, saves
   ([Saves and backups](saves-and-backups.md#saves)), places (Documents in OneDrive, under 5 GB free on a drive, the game
   on a network or removable drive), caches, sessions and script errors. Before Play only installed files and
   compatibility are checked. A check that fails keeps the last findings instead of reading as passed.
2. **State** (`Health.StateOf`). *Blocked* when an error with no fix stops Apex Radiance or its ASI loader
   (`Health.StopsApex`); *Needs your attention* for any other finding that is not information; *Ready to play*
   otherwise. The crystal's colour runs from green (nothing) through amber to red (Blocked).
3. **Tiles.** Each shows *All good*, *N to look at*, *N ignored* or (Mods only) *Not checked*, and opens its page.
4. **Fixes.** The compatibility checks fix what they can: a second Apex Radiance or an old combined build moved aside,
   a second ASI loader moved aside (the first kept), a mod ASI found in two places, a missing `Mods\Resource.cfg`
   created, `Resource.cfg` lines added for mods that need them (a copy kept as `.apex-backup`), the RTX Remix bridge's
   `client.targetProcess` and `dxvk.conf` keys corrected. Others are reported with a link. Each fix has its own restore
   point ([Components](../installer.md)). Suggestions (the game's Edge Smoothing, the renderer, Apex's recommended
   settings) have their own *Apply*.
5. **Fixes before Play.** When the game folder can be written without a prompt, Play lists every compatibility finding
   that has a fix and is not a suggestion in *Fix the game's files first?* (*Fix and play*, *Play without fixing*,
   *Cancel*). With *Apply safe fixes without asking* on, it fixes them without the question.
6. **Ignore and show again.** An ignored notice moves to an *Ignored* section (the *Ignored* tab for Mods), with Undo;
   ids mask numbers and dates so a notice stays ignored when only a count changes.
7. **Stability.** A timeline of the last sessions (up to 8 entries): normal end, crash, freeze, or no report. Each crash
   or freeze is matched to Windows' event log (Application Error 1000, Application Hang 1002, graphics driver events),
   the game's `xcpt *.txt` crash log, Apex Radiance's crash report and log, and the RTX Remix and DXVK logs of that
   session, and shows its cause with *How we know*. A graphics driver fault within 90 s takes the blame. Two or more
   crashes in the last 5 sessions are a warning; three crash logs in the same place in a week are a pattern.
8. **Script errors** (`ScriptErrors`): error logs written since the last game start (at most a week back) are tied to a
   mod through their stack frames; a mod is named only when it threw the error or made the call that failed.
9. **Caches.** Clearing the game caches is suggested at 2 GB or after a session that did not end normally
   ([Mod tools](mod-tools.md#clear-game-caches)).

## Settings

| Menu label | Key (`settings.json`) | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Apply safe fixes without asking | `FixWithoutAsking` | switch | Off | | Play fixes the fixable compatibility findings without asking; off, it asks; *Repair or remove* can undo them. The earlier `FixSafeAutomatically` (on) is not carried over |

## Compatibility and interactions

- Health's Graphics tile reads the renderer from the scan of `Game\Bin`; its Game settings tile reads `Options.ini` and
  `Resource.cfg`.
- The health card on [Home](home-and-play.md) shows the same state and the first finding.

## Limitations

- A finding's tile is chosen from its English text (`Health.AreaOf`, marked as a shortcut in the code); a new finding
  whose text matches no area lands on the Mods tile.
- Crash logs are read for the last 7 days, at most 400 files of up to 4 MB each.
- The Edge Smoothing suggestion reads `Options.ini` `edgesmoothing`; that every value other than 0 means MSAA is
  unverified in game.

## Technical reference

- `CrashEvidence` reads Windows events by querying `wevtutil.exe` for XML; nothing is found when Windows refuses the
  query.
- `SessionHistory.Check` reads the last 5 of the 20 kept sessions.

## Rejected approaches

None recorded.

## See also

- [installer/README.md](../../../installer/README.md#compatibility-fixes): the full list of compatibility fixes and
  suggestions
- [Validation](../../validation/installer.md)

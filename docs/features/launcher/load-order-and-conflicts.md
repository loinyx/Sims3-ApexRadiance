# Apex Launcher: load order and conflicts

When two mods change the same game data, the game uses the one it reads first. The launcher finds these conflicts,
tells which mod wins, and lets the player give a mod priority without editing `Resource.cfg`. *Load order* (Mods >
Tools) lists the packages that are on in the order the game reads them, grouped by `Resource.cfg` line, with whom each
mod wins over or loses to. Copies that mean the same to the game are not reported.

## Status

| | |
|---|---|
| Availability | Conflicts compared by what the game reads, *Give priority*, ignore per conflict: Released in 4.1.1; *Load order* page: In development (branch `release/4.1.2`) |
| Default | Always available |
| Menu | Mods > Tools > Load order; a mod's *Conflicts* tab; Health > Mods |
| Configuration | `conflicts.json`, `hook-table.csv`, `settings.json` `IgnoredConflicts` ([data files](data-and-files.md)) |
| Source | [`MainWindow.ModOrder.cs`](../../../installer/ApexSetup/MainWindow.ModOrder.cs), [`ModConflicts.cs`](../../../installer/ApexSetup.Core/ModConflicts.cs), [`ModConflictsSame.cs`](../../../installer/ApexSetup.Core/ModConflictsSame.cs), [`ModConflictsHooks.cs`](../../../installer/ApexSetup.Core/ModConflictsHooks.cs), [`ModConflict.cs`](../../../installer/ApexSetup.Core/ModConflict.cs), [`ConflictCheck.cs`](../../../installer/ApexSetup.Core/ConflictCheck.cs), [`ModPriority.cs`](../../../installer/ApexSetup.Core/ModPriority.cs) |

## The problem

The game reads packages in an order set by `Mods\Resource.cfg` and file names, and silently keeps one version of each
resource. A mod that loses does nothing, or half of it works; the player sees neither the order nor the loser.

## How Apex Radiance solves it

### The order

`ModConflicts.ParseResourceCfg` reads `Mods\Resource.cfg`: `#` starts a comment, `Priority N` applies to the
`PackedFile` lines after it, and `*` matches within one folder level. `ModConflicts.LoadKey` orders packages by a higher
`Priority` first, then the `PackedFile` line that reads them, then Windows' name order inside one line. The *Load order*
page shows each line as a heading with its priority and the packages under it with their place; packages no line reads
come last, under *Not read by the game*. *All mods* or *Only mods that clash* and a search narrow the list; *See
conflicts* opens the mod's *Conflicts* tab. Without `Resource.cfg` the page says the game reads no mods.

### Conflicts

`ConflictCheck` scans the packages that are on and the ASI plugins and caches the result in `conflicts.json`, keyed by
each file's size and time and the rules' version; a scan longer than 60 s finishes in the background. A conflict is:

- the same resource key in two or more mods with a different meaning;
- two core mods replacing one game assembly, or one assembly name in different builds;
- the same interaction registered by two mods;
- two ASI plugins hooking the same address in the game's exe (from a hook table shipped with the launcher and refreshed
  from Sims 3 Modernization's repository, `ModConflictsHooks.cs`).

"Same meaning" (`ModConflictsSame`) compares what the game reads: XML tuning by elements and attributes (layout and
comments ignored), string tables by text id, settings files by section, name and value (`1` = `1.0` = `1.0f`), window
layouts without their size; everything else byte for byte. Identical copies are no conflict.

Health and the *Conflicts* tab rank each conflict (`ModConflict.ConflictLevel`): *Fix* (assembly versions, core mods,
native hooks), *Look* (the same game data) and *Small* (a menu command, a translation, an add-on, a mod set to load
first, pictures or text only). Small ones do not count in Health's state. A mod that another mod replaces completely is
marked as likely doing nothing. Conflict ids stay the same across mod versions, so an ignored conflict stays ignored.

### Give priority

On a mod's *Conflicts* tab, *Give priority* (`ModPriority.Moves`) moves files between folders that `Resource.cfg`
already reads (for example `Overrides` and `Packages`), with the fewest moves that make the mod win. `Resource.cfg`
itself is never edited. The moves happen all together or not at all, with Undo. Locked mods, mods that are off and
packages no line reads cannot be given priority.

## Settings

None. Ignored conflicts are kept in `settings.json` (`IgnoredConflicts`) and shown again from Health.

## Compatibility and interactions

- The conflict scan is a port of Sims 3 Modernization's compatibility scan.
- A merged file is scanned as one package; `ccmerged.package` is never a conflict.

## Limitations

- The order between `Priority` values and lines follows the game's code; the order of files inside one `PackedFile`
  line (Windows' name order) is unverified in game, so the winner there is "likely".
- *Give priority* works only with the folders `Resource.cfg` already reads; with a single line it has nothing to move
  between.

## Technical reference

- `LoadKey` = (minus `Priority`, line index, upper-case name with `/` as `\0`).
- Without `Resource.cfg` the scan assumes the game's default: `Priority 500` and `*.package` up to six folders deep.
- The rules table for behaviour areas (`ModConflicts.json`) produces warnings that are not shown.

## Rejected approaches

None recorded.

## See also

- [Mods](mods.md), [Health](health.md)
- [Validation](../../validation/installer.md)

# Mod-vs-mod conflict scan

Lists which installed mods clash with each other, by fixed rules only. It reads the Mods folder and `Game\Bin`, writes
nothing there and uses no network. The scan lives in `installer/ApexSetup.Core/ModConflicts.cs`, the rule table in
`installer/ApexSetup.Core/ModConflicts.json` (embedded). It is a port of sims3-modernization's compat-scan with the same
rules and rule table.

```powershell
dotnet run --project tools/mod_conflicts -- [<folder>...] [--mod-folders] [--describe] [--json]
    [--bin <Game\Bin>] [--asm <folder of game dlls>] [--game <install folder>] [--rules <json>] [--hooks <csv>]
```

Without a folder it scans the detected game's `Mods` folder and `Game\Bin`. The exit code is 1 when there is a conflict.

| Level | Rule |
|---|---|
| CONFLICT resource key | One key (type:group:instance) in packages of two or more mods with different bytes, with the likely winner |
| CONFLICT core mod | Two or more mods carry a game assembly (`Sims3GameplaySystems`, `UI`, ...) that is not the game's build |
| CONFLICT assembly | One script assembly name with different builds (old or duplicate copies) |
| CONFLICT interaction | Two or more builds store the same game `Singleton` field: they replace the same interaction |
| CONFLICT native hook | Two ASIs whose hook tables (`--hooks`, lines `<file name regex>,<hex address>`) share a `TS3W.exe` address |
| WARNING | Two or more mods with write-type calls, event listeners or native strings in one area of the rule table, with what could happen |
| INFO | Identical copies, versions or alternatives of one mod, and with `--describe` the readme's first line and the links inside a mod (printed, never fetched) |
| unknown | Archives that do not extract, ASIs without a hook table, assemblies or method bodies that do not decode |

The likely winner follows `Resource.cfg`: higher `Priority` first, then directive order, then name order inside one
`PackedFile` pattern. The first two follow the game's registration code; the name order is unverified in game. `.rar`
and `.7z` archives are extracted with Windows' `tar.exe` into a temporary folder after their listing passes the checks
(no absolute paths, no `..`, no links, at most 20,000 entries and 2 GB); the folder is deleted when the scan ends.
WARNING is a static hint: a call in a rarely used path counts as much as one in a hot loop.

`--mod-folders` treats every first-level folder as one mod, so versions of one mod side by side are INFO, not conflicts.
The tests are in `installer/ApexSetup.Tests` (`ModConflictsTests.cs`, packages and assemblies built in the test).

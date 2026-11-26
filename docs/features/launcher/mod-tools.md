# Apex Launcher: mod tools

The Mods screen's *Tools* menu holds the jobs a player does with many mods at once: *Merge mods* writes several packages
as one file the game loads faster, *Find custom content* tells which mods a Sim, lot, save or list needs and which are
missing, *Load order* shows which mod the game reads first ([Load order and conflicts](load-order-and-conflicts.md)),
*Find a problem mod* finds the mod behind a problem by testing half of the mods at a time. The game's caches are
cleared from Saves > *Game caches* ([saves and backups](saves-and-backups.md#game-caches)). The menu also opens the game's Mods, Saved Sims,
Library and Worlds folders, and a package in s3pe when it is installed.

## Status

| | |
|---|---|
| Availability | Merge and split, Find a problem mod: Released in 4.1.0; Find custom content, Clear game caches by group, merges edited in one go: Released in 4.1.1; Load order: Released in 4.1.2; game caches moved to Saves: Released in 4.1.3 |
| Default | Always available |
| Menu | Mods > Tools |
| Configuration | `hunt.txt`, `cache-backup\`, `logs\` ([data files](data-and-files.md)); `merge.json` in each merge's folder |
| Source | [`MainWindow.ModsMenu.cs`](../../../installer/ApexSetup/MainWindow.ModsMenu.cs), [`MainWindow.ModTools.cs`](../../../installer/ApexSetup/MainWindow.ModTools.cs), [`MainWindow.ModFind.cs`](../../../installer/ApexSetup/MainWindow.ModFind.cs), [`MainWindow.MergeDialog.cs`](../../../installer/ApexSetup/MainWindow.MergeDialog.cs), [`MainWindow.MergePanel.cs`](../../../installer/ApexSetup/MainWindow.MergePanel.cs), [`ModMerge.cs`](../../../installer/ApexSetup.Core/ModMerge.cs), [`ModMergeTakeOut.cs`](../../../installer/ApexSetup.Core/ModMergeTakeOut.cs), [`ModMergeUpdate.cs`](../../../installer/ApexSetup.Core/ModMergeUpdate.cs), [`ModFind.cs`](../../../installer/ApexSetup.Core/ModFind.cs), [`ModHunt.cs`](../../../installer/ApexSetup.Core/ModHunt.cs), [`GameCaches.cs`](../../../installer/ApexSetup.Core/GameCaches.cs) |

## The problem

Thousands of small packages make the game slow to start; merging them by hand loses track of what is inside and cannot
be undone safely. A downloaded Sim or lot shows missing items with no hint of which mod they came from. A broken mod is
found only by moving half of the folder out and back, again and again. Stale caches make items look wrong after mods
change.

## How Apex Radiance solves it

### Merge

1. **Plan** (`ModMerge`). The chosen packages are read once. A resource held identically by several packages goes in
   once. Mods that change the same game data in different ways stay out until the player puts them in and orders them
   (the first in the order wins). Script mods stay out unless included. Name maps are joined. The output is split into
   parts of 1 GB (`PartBytes`; *Name 1*, *Name 2*), and mods that clash share a part.
2. **Run.** Check, Write (every stored byte copied as it is), Verify (the new file read back and every resource hashed
   against the originals), Move originals, Finish (rename into place). Cancel works until the move; after that a
   failure rolls back. Each run writes `logs\merge-<time>.log` in the data folder.
3. **Where things go.** The originals move to `Merged (Apex Launcher)\<name>` next to Mods, which no `Resource.cfg`
   reads, with `merge.json` recording the merged file, its SHA-256 and the originals. A merge that is off lives under
   the Disabled folder. Launchers up to 2.1 kept the originals under `Mods\`; they are moved on start.
4. **Edit in one go** (`ModMergeTakeOut`, `MergeDraft`): take mods out (back on, back off, or to the Recycle Bin), add,
   replace, reorder and rename, held as a draft until *Save* or *Discard*. The planned moves are written to
   `merge.json` before any file moves; a merge left with fewer than 2 mods is dissolved.
5. **Newer versions** (`ModMergeUpdate`): a package outside the merge that matches a mod inside by name without its
   version and shared resources is offered as an update; *Later* is remembered per file.
6. **Split** puts the originals back (checked by hash) and the merged file in the Recycle Bin.
7. **Recovery.** A merge or edit interrupted by a crash or a closed launcher is undone at the next start
   (`ModMerge.Recover`); a merge whose file is gone offers its originals back (*Put back*).

### Find custom content

`ModFind` reads a list (`.xml`, `.txt`, `.csv`, `.html`, `.json`, `.log`) or a game file (`.package`, `.sim`, `.nhd`,
`.world`, a save) for resource keys, package names and item names, and sorts each into *Found*, *Off*, *Missing*,
*Comes with the game* and *Installed with the game's launcher*. Items inside a merge name their original mod. The page
has a search and *Save report*.

### Find a problem mod

`ModHunt` takes the player's own packages that are on (at least 2). Each round turns the first half of the suspects off;
the player starts the game and answers whether the problem is still there, which keeps the half that matters. Mods named
by Health's findings go first. The state, including which mods were on before, is in `hunt.txt`, so *Stop*, a crash or
a restart puts every mod back. At the end: *Turn it off* or *Show it*. The page warns not to save the game during a
round.

### Game caches

The caches are cleared from Saves > *Game caches* ([saves and backups](saves-and-backups.md#game-caches)), the page the
Health action opens. `GameCaches.Clean` never deletes: it moves the chosen groups to `cache-backup\<time>` in the data folder. The copy is
dropped after the next game session that ends normally; *Put the caches back* restores it. The groups, each shown only
when it holds a file where noted:

| Group | Files | Chosen at first |
|---|---|---|
| Create a Sim, Scripts | `CASPartCache`, `compositorCache`, `simCompositorCache`, `scriptCache` packages | yes |
| Worlds, Thumbnails | `WorldCaches\*.package`, `Thumbnails\*.package` | Worlds yes, Thumbnails no |
| Online (when not empty) | `socialCache.package`, `FeaturedItems\*`, `IGACache\*` | no |
| Downloads index | `DCCache\dcc.ent`, `DCCache\missingdeps.idx`, `SavedSims\Downloadedsims.index` | yes |
| Unfinished content files (when not empty) | `DCCache\*.dbc.tmp`: a content database the game wrote half way; `TS3W.exe` reads only `dcdb*.*bc` | yes |
| Backups of installed content (when not empty) | `DCBackup\*.package` except `ccmerged.package` | no |
| Old error logs | script error logs Health has read, and the game's `*.log` except `DeviceConfig.log` (Health reads the graphics card from it) | yes |

The content database itself (`DCCache\dcdb*.dbc`) and `DCBackup\ccmerged.package` are never touched.
Health suggests it when the caches pass 2 GB or after a session that did not end normally.

## Settings

None.

## Compatibility and interactions

- A merged file counts as one mod in [profiles](mods.md#profiles); a merge made after a profile follows its mods when
  they all agree.
- *Open in s3pe* asks before opening a merge, since changes saved there are lost when the merge is edited or split.
- Clearing the caches is refused while the game runs.

## Limitations

- A merged file is at most 2 GB (`int.MaxValue`, the 32-bit game) and 200,000 resources.
- *Find custom content* reads at most 16 MB of text and 20,000 items.
- Not yet verified in game: a package merged by 2.3; the merge on a hard disk and with a player's own thousands of mods
  ([validation](../../validation/installer.md#open-checks)).

## Technical reference

- `ModMergeSafe.cs` holds the journal and the recovery; `ModMergeChoices.cs` the clash choices; `MergeDraft.cs` the
  draft of an edit.
- `GameCaches.Forget` drops the cache copy after a normal session (`SessionHistory`).

## Rejected approaches

None recorded.

## See also

- [Mods](mods.md), [Load order and conflicts](load-order-and-conflicts.md)
- [Validation](../../validation/installer.md)

# Apex Launcher: saves and backups

The Saves screen lists the game's saves with their household photo, world, size and backups, and the saves the player
kept out of the game or that are gone from it, and *Game caches*, the caches the game makes again by itself for every
save. Each save can be renamed, duplicated, kept out of the game, deleted to
the Recycle Bin and cleaned of the pictures and memories that make it large. Backups are zips of a save's folder, made
on request or automatically when the game closes, before Play or once a day; each can be restored over the save or
opened as another one, named, pinned, checked, exported and imported. Every change that moves a save offers Undo.

## Status

| | |
|---|---|
| Availability | Saves with their household photo and a backup: Released in 4.1.0; cleaning, rename, duplicate, kept-out and gone saves, automatic backups, restore, import and export: Released in 4.1.1; Game caches, the Clean panel's parts as rows, copies of a save inside it: Released in 4.1.3 |
| Default | Automatic backups off |
| Menu | Saves |
| Configuration | `backups.json`, `save-stars.json`, `save-sizes.json` ([data files](data-and-files.md)) |
| Source | [`MainWindow.Saves.cs`](../../../installer/ApexSetup/MainWindow.Saves.cs), [`MainWindow.SaveDialogs.cs`](../../../installer/ApexSetup/MainWindow.SaveDialogs.cs), [`MainWindow.SaveBackups.cs`](../../../installer/ApexSetup/MainWindow.SaveBackups.cs), [`Saves.cs`](../../../installer/ApexSetup.Core/Saves.cs), [`SaveInfo.cs`](../../../installer/ApexSetup.Core/SaveInfo.cs), [`SaveHealth.cs`](../../../installer/ApexSetup.Core/SaveHealth.cs), [`SaveClean.cs`](../../../installer/ApexSetup.Core/SaveClean.cs), [`SaveContents.cs`](../../../installer/ApexSetup.Core/SaveContents.cs), [`SaveCopies.cs`](../../../installer/ApexSetup.Core/SaveCopies.cs), [`SaveBackups.cs`](../../../installer/ApexSetup.Core/SaveBackups.cs) |

## The problem

A Sims 3 save is a folder that grows with every portrait, thumbnail and memory the game keeps, and the game keeps only
one `.backup` of it. A save broken by a mod or a crash has no older copy, and renaming or copying a save by hand easily
leaves the game's files out of step.

## How Apex Radiance solves it

### Saves

- **The list** (`Saves.List`): every `*.sims3` folder in `Documents\...\Saves`, newest first ("last played" is the
  newest file in it, `.backup` and `.tmp` files left out). The side list groups them as *In the game*, *Kept out of the
  game* and *Gone from the game*, under a *Backups* group (*All backups*, *Automatic* on or off) and a search.
- **Details** (`SaveInfo.Read`): world, household and description from the save's `Meta.data`, the household photo
  (resource type `0x6B6D837E`), last played, size, backups and the last backup.
- **Save health** (`SaveHealth`): a save over 300 MB, one that grew by more than half in 30 days, one without a backup
  for 14 days, or one missing its world file or `Meta.data`, is listed in Health's Saves tile. Sizes are kept in
  `save-sizes.json`.
- **Menu:** *Open folder*, *Duplicate save* (the next free name, checked file by file), *Rename save* (the folder, its
  `.backup`, its backups, stars, size history and gallery household move with it; names past 259 characters in the
  path are refused), *Keep out of the game* (moved to `Saves kept (Apex Launcher)` next to Saves, back with *Put back in
  the game*), *Move to the Recycle Bin* (backups stay). A save gone from the game comes back from its newest zip or from
  the game's `.backup` (*Bring back*), or is deleted with what is left of it.
- Every change is refused while the game runs: "Close The Sims 3 to clean, back up or restore your saves."

### Cleaning

`SaveClean` removes, by the player's choice, each part on its own row with its size (*Empty* and its switch off when
there is nothing to take): *Sims' portraits*, *Lot and household thumbnails*, *Repeated memories* (one of each
stays), *All memories* and *Selfies, cards and photo booth* (only the pictures the player chose to keep stay), and
*Copies of the save inside it* (`.sims3` folders nested in the save, which the game does not read; measured, in the
backup zip and undone with it). *Unused space* in the files is always reclaimed. Game data, lot textures, the save's own household picture and starred pictures (*Choose the pictures that stay*,
`save-stars.json`) are never removed. It rewrites the world `.nhd` files and `TravelDB.package`; a package with a part
it does not know refuses the whole save. The plan is exact to the byte before anything is written, and a gain under
50 KB is not offered.

1. A full, checked zip backup (*Before cleaning*).
2. Each new file is written as `.apex-new` and read back: header, index and every kept resource's bytes compared with
   the original. The file time is kept.
3. The files swap through `.apex-old`; any failure puts every original back.
4. *Undo* restores the zip, unless the save was played since.

*Clean several* cleans more than one save, each with its own backup first.

### Game caches

A place in the Saves side pane (*Look after your saves*) with the game's caches, grouped by when to clear them (*When
something looks wrong*: Create a Sim, Scripts, Worlds, Thumbnails, Downloads index, Online) and by *Disk space*
(unfinished content files, backups of installed content, old error logs). Each group is a row with its size and a
switch; *n files* opens its files, the ten largest first with *Show more*, each a checkbox (an unticked file stays),
with *Select all* and *Clear all*. *Free up* moves what is chosen aside; *Put the caches back* restores the last
copy. The groups, their files and what is never touched: [mod tools](mod-tools.md#game-caches).

### Backups

- **Format.** `<save> yyyy-MM-dd HH.mm.ss.zip` holding the `<name>.sims3` folder, in `Saves backups (Apex Launcher)` in
  the game's Documents folder or a folder the player picks. The zip is written as `.part`, reopened, compared file by
  file by hash (when *Check each backup* is on, and always before a clean or a restore), then renamed. The zip's comment
  holds the reason (automatic, made by the player, before cleaning, before restoring), an optional name (up to 120
  characters) and a pin; `apex-mods.txt` inside lists the packages that were in `Mods\Packages` (merges expanded), so a
  restore can say which mods from that day are not on now. It is never written into the restored save.
- **Restore** (`SaveCopies.Restore`): *Replace the current save* (the current one becomes a *Before restoring* backup,
  with Undo) or *Open as another save*. The zip is unpacked into a work folder and checked before the folders swap; the
  old folder goes to the Recycle Bin.
- **Actions:** *Give it a name*, *Pin so it is never deleted*, *Check this backup* (CRC of every entry, a world file
  present), *Export* (copied and compared by SHA-256), *Open as another save*, delete to the Recycle Bin. *Import* takes
  a zip with one save and adds it under the next free name.
- **All backups** lists every backup with filters by save and by reason (automatic, made by you, before cleaning,
  before restoring, by the game) and actions for a selection.
- **Folder.** A new folder receives a checked copy of every zip before they leave the old one; any failure rolls back. A
  folder inside Saves or Mods, or one another game uses, is refused.

### Automatic backups

When chosen, a backup is made of each save that changed since its newest zip: 10 s after the game closes, before Play
(a dialog with *Play without waiting*), or once a day (first check 60 s after start, then every 10 minutes while the
launcher is open and the game closed). Incomplete saves are skipped. Each save can have its own times (*Only for this
save*). Past the number to keep, and then past the space limit, the oldest automatic backups go to the Recycle Bin; the
ones the player made, named or pinned, and each save's newest, never do.

## Settings

| Menu label | Key (`backups.json`) | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| When the game closes | `OnClose` | switch | Off | | A backup of each save played lately that changed |
| Before playing | `BeforePlay` | switch | Off | | The same before the game starts; *Play without waiting* skips it |
| Once a day | `Daily` | switch | Off | | The same once a day, while the launcher is open and the game closed |
| Backups of each save | `Keep` | choice | 5 | 1, 3, 5, 10, 20 (stored 1 to 50) | How many automatic backups of a save stay |
| Space limit | `LimitGb` | choice | 10 GB | 2, 5, 10, 20, 50 GB, No limit (0) | Past it the oldest automatic backups leave |
| Folder | `Folders` | folder | `Saves backups (Apex Launcher)` in the game's Documents folder | | Where the backups are, per Documents folder |
| Check each backup | `Check` | switch | On | | Compares the zip with the save after copying |
| Remember the mods that are on | `RememberMods` | switch | On | | Writes `apex-mods.txt` into each backup |

Per-save choices are kept under `Saves` in the same file.

## Compatibility and interactions

- A save renamed by the launcher keeps its backups, Gallery household and stars; one renamed outside the launcher
  shows its old backups under *Gone from the game*.
- Home shows how many of the newest saves have no backup ([Home and Play](home-and-play.md)).
- The [uninstall](uninstall.md) never touches saves or their backups.

## Limitations

- The save health limits (300 MB, 50% in 30 days, 14 days) are marked unverified in the code.
- Keeping a save out of the game is refused when the Saves folder is on another drive.
- Not yet verified in game: a save restored from a backup made by 2.3, a cleaned save; automatic backups when a real
  game closes and before Play; a backup folder on a USB or network drive
  ([validation](../../validation/installer.md#open-checks)).

## Technical reference

- `SaveCopies.Recover` finishes or undoes interrupted work (a swap, a move) at start.
- A backup's name and pin are written by rewriting only the zip's end record (`SaveBackups.Mark`); the file date stays.
- Backups are removed to the Recycle Bin, or deleted on drives without one.

## Rejected approaches

None recorded.

## See also

- [Home and Play](home-and-play.md), [Gallery](gallery.md)
- [Validation](../../validation/installer.md)

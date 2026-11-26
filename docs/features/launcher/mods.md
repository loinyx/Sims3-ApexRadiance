# Apex Launcher: Mods

The Mods screen lists every package and script mod in the game's Mods folder and every ASI plugin in `Game\Bin`, with
what it is, its folder, size, tags and notes, and an on/off switch. A mod's details show its content, its conflicts,
what it replaces and its problems. Files of any kind the game reads (packages, Sims, lots, households, worlds, archives
and folders) can be added from the screen, from a drop or from *Open with*, and go to the place the game reads them from.
Profiles keep which mods are on and switch between sets in one step.

## Status

| | |
|---|---|
| Availability | Released in 4.1.0; columns, filters, details, adding files, profiles, mod page address, *Open in s3pe*: Released in 4.1.1 |
| Default | Always available |
| Menu | Mods |
| Configuration | `mod-notes.json`, `mod-pictures\`, `mod-profiles.json`, `mods-view.txt` ([data files](data-and-files.md)) |
| Source | [`MainWindow.Mods.cs`](../../../installer/ApexSetup/MainWindow.Mods.cs), [`MainWindow.ModDetails.cs`](../../../installer/ApexSetup/MainWindow.ModDetails.cs), [`MainWindow.ModContent.cs`](../../../installer/ApexSetup/MainWindow.ModContent.cs), [`MainWindow.ModInstall.cs`](../../../installer/ApexSetup/MainWindow.ModInstall.cs), [`MainWindow.ModProfiles.cs`](../../../installer/ApexSetup/MainWindow.ModProfiles.cs), [`Mods.cs`](../../../installer/ApexSetup.Core/Mods.cs), [`ModLibrary.cs`](../../../installer/ApexSetup.Core/ModLibrary.cs), [`ModKinds.cs`](../../../installer/ApexSetup.Core/ModKinds.cs), [`ModAdd.cs`](../../../installer/ApexSetup.Core/ModAdd.cs), [`ModProfiles.cs`](../../../installer/ApexSetup.Core/ModProfiles.cs) |

## The problem

The game reads every package that `Mods\Resource.cfg` names and gives no list of them. Turning a mod off means moving
it by hand to a folder the game does not read; a package's file name rarely says what it holds; and a file dropped in
the wrong folder, a duplicate or a package from another game silently does nothing or breaks a save.

## How Apex Radiance solves it

### The list

- **What is listed** (`Mods.List`): every `*.package` under `Documents\...\Mods` (packages kept by a merge left out), and
  every `*.asi` in `Game\Bin`, `Bin\scripts` and `Bin\plugins`. A mod's place is its path as the game sees it, the same
  whether it is on or off.
- **On and off.** Off moves the file to `Mods\Disabled (Apex Launcher)\<same relative path>`, a folder no `Resource.cfg`
  line reads; on moves it back. A name taken in the target gets " (2)" and up. Switching is refused while the game runs,
  and when a `PackedFile` line would also load the Disabled folder.
- **Locked mods.** Files the launcher installed from Components and ASI plugins carry a lock; their button is *Open in
  Components*.
- **What it is.** A coarse kind from the resource types in the package index (script, Create a Sim, objects, lighting,
  sound, textures, tuning, interface; `Mods.KindOf`) and fine kinds read from the catalog data (`ModKinds.cs`, in the
  groups Gameplay, Create a Sim, Buy mode, Build mode, Look and sound, Other). The player can set it by hand (*Back to
  detected* resets it).
- **Default replacements** are mods that replace an object, a Create a Sim part or a texture the game ships
  (`Replacements.IsDefault`, against an index of the game's own resource keys); a mod that only overrides tuning or
  scripts is not one.
- **Places** in the side pane: All, New since last play, Needs a look (a mod check finding, or a mod that is on in a
  folder `Resource.cfg` does not read), Duplicates (same size and SHA-256), Off, Merged, the player's folders, tags and
  ASI plugins. Profiles sit at the top of the pane.
- **Views.** A list with Name, Type and Size columns (click a header to sort; also by folder, newest added and problems
  first) or a grid of square tiles. Narrow windows drop the Type and Size columns and turn the pane into a menu.
- **Search and filters.** The search covers the name and path, notes, tags, creator, kinds and content names. Filters
  combine as "any value inside a group, every group in use": file type, what it is, state (on, off, needs a look, new,
  duplicate, default replacement), tags, creator, added (today, 7, 30, 90, 182 or 365 days, over a year) and size (over
  10, 50 or 200 MB).
- **Notes, tags, picture, creator and page address** are kept in `mod-notes.json` by the mod's place and follow the mod
  when the launcher moves or renames it. Tags take one of 8 colours. The creator is guessed from the file-name prefix
  when two or more mods share it or it is a known maker; the player's choice wins.
- **Folders.** Create, rename, move and delete folders; remove a mod to the Recycle Bin. Paths longer than 259
  characters are refused (the game is 32-bit). Every change offers Undo.
- **Mod checks** (`ModChecks.cs`, `IModCheck`): today one check, a script mod that references an assembly no installed
  package provides.

### Details

A mod's pane shows only the tabs that have something: *Mods* (for a merge), *Overview* (size, added, folder, file,
creator, what it is, tags, *Open in s3pe*, *Show in Explorer*), *Content* (items and resources with a search; resources
export as text), *Conflicts* (mods it replaces and is replaced by, with *Give priority*, see
[Load order and conflicts](load-order-and-conflicts.md)), *Replaces* (for a default replacement), *Problems* and
*Notes*. A mod that is off and holds items warns that saving the game removes them. The footer turns the mod on or off,
splits a merge, or moves it to the Recycle Bin.

### Adding files

*Add mods*, a drop from Explorer or *Open with* on a `.package` (`ModAdd.cs`, `ModInstall.cs`):

| File | Goes to |
|---|---|
| `.package` (a mod) | The chosen Mods folder, else `Mods\Packages`; `Resource.cfg` is created when missing |
| `.package` holding an export, lot or household | `Documents\...\Library` |
| `.sim` | `Documents\...\SavedSims` |
| `.world` | `Game\GameData\Shared\NonPackaged\Worlds` (through the [administrator copy](elevation.md) when needed) |
| `.zip`, `.rar`, `.7z`, a folder | Opened (one archive inside another at most, 20,000 files and 2 GB unpacked) and each file sorted as above |

- A package must be DBPF 2.0 with its index inside the file; protected packages and Sims 4 packages are refused. A
  package that looks damaged shows unticked with the reason and is added only when ticked; a script mod carries a
  warning to add it only from a trusted page.
- A file with the same bytes as an installed one (also inside a merge) is *already there*. A file with the same name and
  other bytes is offered as a replacement, ticked when the two share resource keys. A world, a locked file and a
  merged file are never replaced.
- Each file is copied under a temporary name, checked against the hash that was inspected, and renamed into place,
  never over a file; a replaced file goes to the Recycle Bin. Undo sends the added files to the Recycle Bin.
- A `.sims3pack` (also inside a zip) is opened: each package of custom content in it (objects, clothes, hair,
  patterns) is taken out as a mod named after its item (the package's manifest `PackageTitle`, else the Sims3Pack's
  name) and checked as any package; a store item's protected package (`DBPP`) has its header written again from the
  index at the end of the file. Lots, worlds and Sims are listed as left out, with a pointer to the game's own
  launcher. The Sims3Pack itself stays where it was. Programs (`.exe`, `.dll`, `.asi`, scripts) are refused.

### Profiles

A profile keeps the on/off state of every package of the player's (launcher-installed mods are never part of one) in
`mod-profiles.json`. Switching turns mods on and off all at once or not at all, warns that saves can lose items, offers
to save the mods that are on now as a profile first (*Before X*), and has Undo. Mods added after a profile was made are
left as they are. Moves, renames, merges and splits update every profile. Profiles cannot be switched during *Find a
problem mod* or while the game runs.

## Settings

None. The view (place, list or grid, sort, collapsed groups) is remembered in `mods-view.txt`.

## Compatibility and interactions

- Mods turned off by the launcher are turned on again by the [uninstall](uninstall.md).
- The Mods screen's tools are on [Mod tools](mod-tools.md) and [Load order and conflicts](load-order-and-conflicts.md).
- [Health](health.md) lists the mod checks and conflicts on its Mods tile.

## Limitations

- Only packages under the Mods folder and ASIs in `Game\Bin` are listed; Sims3Pack content installed by the game's own
  launcher (`DCCache`) is not.
- Duplicates larger than 512 MB are not hashed.
- Only the first 200 files of an add get a row in the dialog.

## Technical reference

- `Dbpf.cs` reads package headers and indexes; `ModInspect.cs` and `ModContents.cs` read names from string tables,
  object definitions and Create a Sim parts within fixed limits (`MaxTables` 60, 400,000 strings and names).
- `DefaultReplacements.cs` keeps a hashed index of every resource key the game ships.
- Archives are unpacked with Windows' `tar.exe`, then 7-Zip when installed; entry paths are stripped and nothing
  extracted is run.

## Rejected approaches

- Installing `.sims3pack` files through the game's installer (lots, worlds, Sims): removed; they are left to the game's
  own launcher. Custom content inside one is added as mods.

## See also

- [Mod tools](mod-tools.md), [Load order and conflicts](load-order-and-conflicts.md)
- [Validation](../../validation/installer.md)

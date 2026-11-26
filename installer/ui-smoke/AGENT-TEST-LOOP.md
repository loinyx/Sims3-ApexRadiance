# Apex Launcher: a test loop for a coding agent

Instructions for an AI coding agent that tests Apex Launcher on a tester's Windows PC, over and
over, to find bugs. The Mods screen matters most, but every screen is in scope. Read the whole file before starting.

The agent reports what it finds; it does not change the launcher's code or push to the repository unless the tester
asks.

## 1. Rules that come before everything

- **The player's things are never touched.** Use the sandbox in section 3 for anything that moves, renames, merges,
  tags or removes mods. The real `Documents\Electronic Arts\The Sims 3` (Mods, Saves, Library) and the game folder
  may only be read.
- **The Sims 3 is closed** (`TS3W.exe`, `TS3.exe`) before any run. If it is open, wait; never close it for the
  player.
- **One launcher at a time.** A second start hands its files to the open launcher and exits. Before a run, ask the
  tester to close their launcher, or check that no `ApexLauncher.exe` is running.
- **Nothing on the tester's screen they did not ask for.** Do not press buttons that open Explorer windows
  ("Open log folder", "Open in Explorer", "Show file"): check that they exist, do not click them.
- Captures are of the launcher window only (`PrintWindow` on its handle), never the full screen.
- A capture that comes out all black usually means the display is asleep or locked; read the window through UI
  Automation instead and say so in the report.

## 2. Get the build

Either the test build attached to the draft release the maintainer shared (`ApexLauncher.exe`), or build it from the
pull request branch (`release/4.1.0`):

```
dotnet publish installer/ApexSetup -c Release -o <out>
```

Note the commit you test (`git rev-parse --short HEAD`) or the release's commit in every report.

## 3. The sandbox

The launcher reads these environment variables. Set them in the process that starts it:

| Variable | What it does |
|---|---|
| `APEX_SETUP_DOCUMENTS=<folder>` | Uses this folder as `Documents\Electronic Arts\The Sims 3` (Mods, Saves, Library). |
| `APEX_LAUNCHER_LOCALAPPDATA=<folder>` | Keeps the launcher's own data (notes, tags, logs, settings) here. |
| `APEX_LAUNCHER_PORTABLE=1` | Runs where it is: no install, no shortcuts, no Installed apps entry. |
| `APEX_SETUP_DRY_RUN=1` | Never writes to the game folder or the drivers (Play, Components and Repair refuse). Mods in the sandbox still change. |
| `APEX_LAUNCHER_LANG=<code>` | Forces a language (`en`, `pt`, `de`, `ja`...). |

Build a sandbox for each round: a new temporary folder with `Mods\Packages\...` holding **copies** of a few packages
(take them from the tester's Mods folder by copying, never moving), a `Mods\Resource.cfg` that loads
`Packages/*.package`, `Packages/*/*.package` and `Packages/*/*/*.package`, and an empty data folder. Pass the game folder
as the first argument to the exe. `installer/ui-smoke/Run-ModsHarness.ps1` shows exactly how.

## 4. The ready-made checks (run first, every round)

```
powershell -ExecutionPolicy Bypass -File installer\ui-smoke\Run-UiSmoke.ps1 -Exe <exe> -Game "<game folder>" -Runs 1 -Quick
powershell -ExecutionPolicy Bypass -File installer\ui-smoke\Run-ModsHarness.ps1 -Exe <exe> -Game "<game folder>" -Packages "<folder of .package files>" -Out <temp folder>
dotnet run --project installer/ApexSetup.Tests
```

- The smoke run drives every button in a dry run and fails on any new error in the log.
- The Mods harness checks 14 Mods features on disk in a sandbox. A step that types text needs the launcher in front;
  if the tester uses the PC at the same time, a step can fail for that reason alone: run it again before reporting.
- The unit tests end with `ALL PASSED`. A file test that fails once and passes on the next run is an antivirus lock;
  report it only if it repeats.

## 5. The loop

Repeat until the tester stops you or a round finds nothing new three times in a row:

1. New sandbox (section 3), start the launcher, wait for the window.
2. Pick the next area from section 6 (start with Mods; vary the order between rounds), and work through it with UI
   Automation: names come from `AutomationProperties.Name` or the visible text; a dialog's buttons are the template
   parts `PrimaryButton`, `SecondaryButton` and `CloseButton` (by `AutomationId`, never the window's caption Close).
3. After every action check three things:
   - **on disk**: the files in the sandbox are what the action says (moved, renamed, merged, restored, nothing lost);
   - **in the log**: `<data folder>\Apex Launcher\ApexLauncher.log` has no new exception;
   - **on screen**: a capture of the window; look for cut text, overlap, misalignment, wrong colours, empty panels.
4. Try the unhappy paths too: cancel halfway, repeat an action twice quickly, act on a file another program holds
   open, very long names, names with accents and emoji, a folder dragged into itself, an empty folder, 0-byte and
   damaged `.package` files, a `.zip` with mixed content, two copies of the same mod.
5. Resize the window between rounds (760×560 up to full screen) and change the language now and then.
6. Close the launcher, compare the sandbox with what you expected, delete the sandbox.

## 6. What to cover

**Mods (most important)**

- The side pane: Library places (All mods, New, Default replacements, Needs a look, Duplicates, Turned off), folders,
  tags, Game plugins (ASI). Clicking each one shows its mods; counts match the list.
- List and grid views, search (name, folder, notes, tags, creator, type), every filter group (file type, what it is:
  Create a Sim, Buy mode, Build mode with walls, floors, fences, roofs, terrain paints..., state, tags, creator, date
  added, size), sorting, Select all and Clear.
- Selecting: click, Ctrl and Shift, the row checkboxes, the selection bar.
- A mod's details: picture (add and remove), rename in place and Undo, tags (make, give, take off, edit, delete),
  notes kept after leaving and coming back, creator (guessed and typed), type, folder and Move, "Replaces in the
  game" for default replacements, "Content" for objects and Create a Sim parts
  (and the resource list: search, copy, save as text), "Conflicts" (give priority and Undo, See in Health).
- On and off for one mod, many mods, a whole folder; turning a mod with items off warns about saves.
- Folders: make, rename, drag a mod or a folder into another, delete an empty one; a folder that cannot be deleted
  says why.
- Merge: the plan, the run with its progress and its log at the end, the check of the result, splitting back, taking one mod out of a merge; the
  originals come back identical (compare file hashes).
- Profiles (the button on top of the side pane): save what is on, switch, Undo, rename, copy, update, delete; a mod added
  later stays as it is, a mod moved or renamed in the launcher follows, a mod held open by another program leaves every
  mod as it was; refused while the game or a hunt runs.
- Tools: each entry, the folder buttons, and an entry that can't be used says why.
- A mod's page: add, open, copy, edit, remove; anything but a web address is refused.
- Add mods: the picker, drag and drop from Explorer, and "Open with" on `.package` and `.zip`; every file is checked
  first; damaged and duplicate files are refused or offered as a copy; a `.sims3pack` is refused (not a mod file).
- The problem-mod hunt: start, answer rounds, stop; every mod is back where it was.
- Remove to the Recycle Bin, with its confirmation.

**Everything else**

- Home: banners, Play (refused in a dry run), the health card, Continue, What's new, pictures.
- Components: the toggles, Stable and Experimental, Apply.
- Worlds and Saves: cards fill the width, pictures load, Copy all.
- Health: Check now, every area, Automatic fixes, Restore points, Last session, the copy-report button.
- What's new, Settings (every row), the language menu, the first-start question (installed or portable).
- Every dialog: the action has the accent colour; removing and deleting are in red; Esc and Cancel change nothing.

## 7. How to report

One report per problem, in plain English, to the tester (they pass it on to the maintainer):

```
Title: <screen>: <what goes wrong, in a few words>
Build: <commit or release>   Window: <width>x<height>   Language: <code>
Steps:
1. ...
2. ...
Expected: ...
Actual: ...
Log: <the exception lines from ApexLauncher.log, if any>
Capture: <file name of the window capture>
Seen: <how many times out of how many tries>
```

Put the worst first: anything that loses or changes a player's file wrongly, then crashes and errors, then wrong
results, then layout and wording. At the end of a session, give a short summary: rounds run, areas covered, problems
found, and what you could not test and why.

# Apex Launcher: validation

What Apex Launcher (Apex Radiance Setup up to 1.2.x) must do, stated as checkable properties. The program is described
in [features/launcher/](../features/launcher/README.md); its Components screen in
[features/installer.md](../features/installer.md).

- It changes only what differs from the player's choice, keeps a restore point before every change, and never touches
  saves, mods put in by hand or `Resource.cfg` on removal.
- It runs as the signed-in user and asks for administrator rights only for a change to a folder or a driver setting the
  user cannot write.
- Mods turned off, moved, merged, split or renamed keep their files, their notes and a unique place; no file of the
  player's is lost on a name clash. An interrupted merge, save clean or restore is undone or finished at the next start.
- A save is changed (cleaned, restored, renamed) only after a checked backup or with Undo; backups are removed only by
  the launcher's own retention rules or by the player, to the Recycle Bin.
- The Gallery only reads the player's pictures and videos; it writes its own `gallery.json`, and a picture is deleted
  only to the Recycle Bin when the player asks.
- Game settings write only the values the player changed and can put every file back.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `installer/ApexSetup.Tests` | `Program.cs`: the install engine on a fake `Game\Bin` and Documents in `%TEMP%` (scan, components, renderer choice, check installation, compatibility fixes, restore points, elevation plans, uninstall, self-update, frame generation, translations). Then one file per area: `ModConflictsTests`, `ModInspectTests`, `DefaultReplacementsTests`, `ModKindsTests` (packages and conflicts), `ModMergeTests`, `MergeDraftTests` (merge, split, take out, recovery), `ModAddTests`, `ModToolsTests` (adding files, folders, duplicates, the problem-mod hunt), `ModProfilesTests`, `ModFindTests` (Find custom content, caches), `HealthTests`, `GameSettingsTests`, `SavesTests` (saves, cleaning, backups), `GalleryTests`, `InstallTests` (installed and portable modes in a temporary profile), `DesignTests` (design system rules read from the source) | `dotnet run --project installer/ApexSetup.Tests -c Release`; prints `ok` or `FAIL` per check and exits non-zero on a failure | .NET 8 SDK; no game, no network |
| `installer/ui-smoke/Run-UiSmoke.ps1` | Starts the published exe in a dry run (`APEX_SETUP_DRY_RUN=1`, portable, nothing written to the game or drivers) and drives every screen's controls through UI Automation: switches, renderer and frame limit lists, Apply, Done, Repair or remove, the licences, all 21 languages, a narrow and a wide window. After each step it checks the launcher still runs and its log has no new error but the dry run's refusals. Puts `language.txt` back | `powershell -ExecutionPolicy Bypass -File installer\ui-smoke\Run-UiSmoke.ps1 -Exe installer\publish\ApexLauncher.exe -Game "<game folder>" [-FakeNv ...] [-FakeAmd ...] [-Runs 3] [-Quick] [-Shots <folder>]`; `-Quick` skips the language cycle; exit code = failed steps | The published exe, a game folder (read only), Windows UI Automation |
| `installer/ui-smoke/Run-ModsHarness.ps1` | Mods screen actions against a sandbox (a temporary Documents folder with copies of a few packages and its own data folder), each checked on disk and captured: turn off and on, a tag, notes, rename and undo, merge and split, start and stop a problem-mod hunt, a new folder, *Open with* from a second launcher, removal to the Recycle Bin | `powershell -File installer\ui-smoke\Run-ModsHarness.ps1 -Exe <ApexLauncher.exe> -Game "<game folder>" -Packages <folder of at least four .package files> [-Out <folder>]`; prints steps, failures and captures | The published exe; the game closed |

Optional read-only runs over real files: `APEX_HEALTH_REAL=1` with `APEX_HEALTH_GAME=<game folder>` (Health and Game
settings), `APEX_FIND_GAME=<game folder>` (Find custom content), `APEX_SAVES_REAL=<a copy of a Saves folder>` (saves,
copied before use). The layout tour (`APEX_LAUNCHER_AUDIT` with `APEX_LAUNCHER_AUDIT_TOUR`) records texts cut short and
controls past the window's edge at several window sizes. Build commands are in [workflow.md](../workflow.md#apex-launcher).

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-26 | `0eb74a39` | `installer/ApexSetup.Tests` | All passed | none |
| 2026-11-26 | `0eb74a39` | `installer/ui-smoke/Run-UiSmoke.ps1 -Quick` (dry run, a data folder of its own) | 79 steps, 0 failures | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `af2b44ab` | `installer/ApexSetup.Tests` | All passed | none |
| 2026-11-25 | `af2b44ab` | `installer/ui-smoke/Run-UiSmoke.ps1 -Quick` (dry run, a data folder of its own) | 79 steps, 0 failures | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `a3c94ef0` | `installer/ApexSetup.Tests` | All passed | none |
| 2026-11-25 | `a3c94ef0` | `installer/ui-smoke/Run-UiSmoke.ps1 -Quick` (dry run) | 79 steps, 0 failures | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `a3c94ef0` | `installer/ui-smoke/Run-UiSmoke.ps1` (full, dry run) | 81 steps passed; the language cycle again ended the launcher (0xC000027B) while UI Automation read it, as on `6a272a14` | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `5ea03f21` | Sixty changes of light and dark in a row on Settings, the gallery, Mods and Components (a test build that switches by itself) | Kept running; before `5ea03f21` the third change closed the launcher (a new Mica at every change) | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `f4c02666` to `a3c94ef0` | Three independent reviews (Mods; Home, Saves and the gallery; install, settings and Health) | 51 findings, all fixed in `709d78bd`, `829c4616` and `7196b72b`; none seen in the app yet | none |
| 2026-11-25 | `43c2f8c2` | `installer/ApexSetup.Tests` | All passed | none |
| 2026-11-25 | `43c2f8c2` | `installer/ui-smoke/Run-UiSmoke.ps1 -Quick` (dry run) | 79 steps, 0 failures | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `43c2f8c2` | `installer/ui-smoke/Run-UiSmoke.ps1` (full, dry run) | Every screen and step passed up to the language cycle; there the launcher closed (0xC000027B in Microsoft.UI.Xaml) after 15 to 20 changes of language while UI Automation read the window. The same happens on `6a272a14`. Sixty changes of language in a row without UI Automation, on Home and on Components, left it running | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `09c42079` | Layout tour (`APEX_LAUNCHER_AUDIT_TOUR`): nine screens at five window sizes, in the 21 languages | Texts cut short fixed: row details take up to three lines, the facts of a save and the profile's name wrap; what is left is the row of mod types, which scrolls | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `43c2f8c2` | `installer/ui-smoke/Run-ModsHarness.ps1` | 6 of 11 steps passed (turn off and on, tag, notes, rename and undo, start a hunt); the others look for the Set folder by an older name and fail from there | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `785921fc` to `43c2f8c2` | Published exe in a sandbox: every screen, the 18 dialogs, menus, messages and the install, dark and light, English and Portuguese | Rendered as expected; captures reviewed | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `5166d9cd` and the review fixes after it | `installer/ApexSetup.Tests` | All passed, with new checks of packages that look damaged and of files replaced between two plans of a merge | none |
| 2026-11-25 | `5166d9cd` and the review fixes after it | `installer/ui-smoke/Run-UiSmoke.ps1 -Quick` (dry run) | 79 steps, 0 failures | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `5166d9cd` and the review fixes after it | Merge of 4000 generated packages (1.9 GB, NVMe): core run, and the launcher in a sandbox | Run 6.6 s to 3.4 s, plan again 1.3 s to 0.4 s, 234,857 progress reports to 53, peak 208 MB, same SHA-256; in the launcher 4.1.0's dialog answered again after about 8 minutes, this one stayed responsive and took 5.7 s | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-25 | `5166d9cd` and the review fixes after it | Published exe in a sandbox: Mods, its details pane and menus, merge dialogs and Health, dark and light, 780 to 1920 wide, English, Portuguese and German | Rendered as expected; captures reviewed | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-24 | `e64890dd` | `installer/ApexSetup.Tests` | All passed, with the new folder-guard and crash-timeline checks | none |
| 2026-11-24 | `1a2b53e6` | The move from Apex Radiance Setup on this PC (separate `%LOCALAPPDATA%`, the old data folder, `ApexRadianceSetup.exe` in a Downloads folder, a copied `TS3W.exe` with the ASI loader) | What's new, Where it lives and Ready rendered; Install copied it, the installed copy opened on Ready, the old copy in Downloads was deleted, Start menu entry made | Windows 11 |
| 2026-11-24 | `a19e0296` | Stability timeline on the player's real logs (read only) | Sessions, crashes and freezes matched to Windows events, RTX Remix and Apex Radiance logs; no crash put on a plugin that was only further down the stack | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-24 | `2f2b4d44` | `installer/ApexSetup.Tests` | All passed | none |
| 2026-11-23 | `2f2b4d44` | Real launcher (2.0.10) on this PC: Home with the picture on top as a gallery picture, the household's photo and none; Components; Health; the first install's list | Rendered as expected; captures reviewed | Windows 11 |
| 2026-11-21 | 2.0.1 build of this change | Card install and update on this PC (test folders, faked drivers): the updates-first card and an update 4.0.12 → 4.0.13 from Play; an update failing on a locked `ApexRadiance.asi`; no internet through a dead proxy; a stand-in `TS3W.exe` running; a new launcher offered with `APEX_LAUNCHER_FAKE_UPDATE` and *Restart now* | The card listed the update and updated; the failure was taken back (4.0.12 stayed) and its card offered Try again and Play anyway; the notices showed in the cards; the launcher replaced itself with 2.0.2 and reopened | Windows 11 |
| 2026-11-20 | 2.0.1 build of this change | Full cycle on this PC, `APEX_LAUNCHER_NO_DETECT=1`, faked drivers, a copied `TS3W.exe` as the only game: install from a download, then *Uninstall* through the Installed apps command with the launcher still open and settings not kept | The open launcher closed; the game folder back to `TS3W.exe` alone; install folder, data, Installed apps entry, shortcuts, Run value and Open with gone. Left: `%ProgramData%\Apex Radiance Setup` (the Smooth Motion record) when the uninstall needs no administrator | Windows 11 |
| 2026-11-20 | 2.0.1 build of this change | Real install on this PC, separate `%LOCALAPPDATA%` and a copied `TS3W.exe` as the game | First install from a download: Apex installed, the question after it, the installed copy started with Installed apps, Start menu, desktop and Open with; a downloaded 2.0.2 replaced a running 2.0.1; an older download only opened the installed copy; an Apex Radiance Setup user (old data folder, `ApexRadianceSetup.exe`) kept language and games and installed | Windows 11 |
| 2026-11-20 | `4fd4114b` | `installer/ApexSetup.Tests` | All passed three runs in a row; file-timing tests (merge, split, caches, uninstall) fail now and then and pass on a rerun | none |
| 2026-11-20 | `4fd4114b` | `installer/ui-smoke/Run-UiSmoke.ps1` (dry run, Steam test game) | 432 steps, 0 failures | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-20 | `4fd4114b` | Published exe in a sandbox: every screen in light and dark, at 1600x1000 and 900x760, Portuguese and English | Rendered as expected; captures reviewed | Windows 11, RTX 4070 Ti SUPER |
| 2026-11-20 | `84cc0fc2` | `installer/ApexSetup.Tests` | All passed (merge and split checks rerun when flaky) | none |
| 2026-11-20 | `84cc0fc2` | Published exe in a sandbox (fake Documents and data, `APEX_LAUNCHER_THEME`): every screen in light and dark, the Gallery's board, viewer, videos and search | Rendered as expected; captures reviewed | Windows 11, RTX 4070 Ti SUPER |

## In-game test plan

1. Upgrade from Apex Radiance Setup 1.2.x: run its *Update Setup*. **Expected:** Apex Launcher installs itself, keeps
   the language and settings, and the game's components are unchanged.
2. A game under `C:\Program Files`: install, update and remove a component. **Expected:** one administrator prompt
   per change; the change and its restore point as with Setup.
3. Play from Home with *Hide while playing* on. **Expected:** the game starts, the launcher waits in the notification
   area and comes back when the game closes; a crash offers *Open this save again*.
4. *Uninstall* from Windows' *Installed apps*. **Expected:** Apex's files leave every known game; saves, mods put in by
   hand and `Resource.cfg` stay; the launcher's shortcuts, data and exe are gone.
5. Change Windows between light and dark with the launcher open. **Expected:** the launcher follows at once.
6. Take a screenshot and record a video with Apex, the folders set to *Apex* and then to a custom folder. **Expected:**
   both appear in the Gallery; the folder buttons open the folders in use.

## Confirmed in game

- Every screen renders in both themes with the Windows accent colour (sandbox captures, `84cc0fc2`).
- A video recorded by Apex (`.mp4`) shows Windows' own frame with a play mark and plays in the viewer.

## Open checks

- Steps 1 to 6 of the in-game test plan on a real install, including the EA app edition. Not yet run for real: the
  update an elevated Setup 1.2.x starts (the restart as the player needs a Windows prompt) and the full uninstall
  (it acts on every game the PC has).
- The merge, split and cache tests fail now and then on this PC and pass on a rerun.
- Dragging a picture onto another to make or join an album, with a real mouse.
- The texts new in 4.1.0 in the 19 languages other than English and Portuguese.
- Home: a save's Play opening the game into that save (`-worldFile`) with a real click, and the crash or low-memory
  state with *Open this save again*.
- The Acrylic and stronger Mica window backgrounds.
- In game: a save restored from a backup, a cleaned save, a package merged by 2.3, the Game settings values, and which of two packages of one folder is in
  use.
- The merge on a hard disk and with a player's own thousands of mods; the question about a package that looks damaged, on
  screen.
- Automatic backups when a real game closes and before Play (once a day was run on a copy of real saves), the pickers of
  Import, Export and the folder, and a folder on a USB or network drive.
- A theme switch with the launcher open after 2.3 (menus and lists), and a contrast theme.

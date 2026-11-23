# Apex Launcher: validation

What Apex Launcher (Apex Radiance Setup up to 1.2.x) must do, stated as checkable properties. The program itself is
described in [features/installer.md](../features/installer.md).

- It changes only what differs from the player's choice, keeps a restore point before every change, and never touches
  saves, mods put in by hand or `Resource.cfg` on removal.
- It runs as the signed-in user and asks for administrator rights only for a change to a folder or a driver setting the
  user cannot write.
- Mods turned off, moved, merged, split or renamed keep their files, their notes and a unique place; no file of the
  player's is lost on a name clash.
- The Gallery only reads the player's pictures and videos; it writes its own `gallery.json`, and a picture is deleted
  only to the Recycle Bin when the player asks.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `installer/ApexSetup.Tests` | Engine, compatibility checks, restore points, elevation plans, uninstall, self-update, mods (on/off, merge, split, take out, hunt, conflicts, notes), saves, worlds, health, gallery data and folders, translations | `dotnet run --project installer/ApexSetup.Tests` | none |
| `installer/ui-smoke/Run-UiSmoke.ps1` | Every screen opens in a portable dry run, no launcher left running | `Run-UiSmoke.ps1` | the published exe |
| `installer/ui-smoke/Run-ModsHarness.ps1` | Mods screen actions against a fake Documents folder | `Run-ModsHarness.ps1` | the published exe |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-23 | `2f2b4d44` | `installer/ApexSetup.Tests` | All passed | none |
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
   area and comes back when the game closes; a crash offers *Start again into this save*.
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
- Home (2.0.10): *Continue* opening the game into the save shown (`-worldFile`), a save picked from *Another save* with a
  real click, and the crash or low-memory state with *Start again into this save*.
- The Acrylic and stronger Mica window backgrounds.

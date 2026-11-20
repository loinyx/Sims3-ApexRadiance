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

- Steps 1 to 6 of the in-game test plan on a real install, including the EA app edition.
- `ui-smoke` on the published 2.0.0 exe.
- Dragging a picture onto another to make or join an album, with a real mouse.
- The newest Gallery texts in the 20 languages other than English.

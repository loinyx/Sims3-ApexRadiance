# Report a problem and screenshots: validation

The feature is described in [features/bug-reports.md](../features/bug-reports.md). It must:

- Write every capture into a new folder, never overwrite or delete one by itself, and complete it with the log,
  settings, crash file and explanation.
- Keep files safe while output is pending: no deletion, no session start or end, retry without repeating the
  measurement.
- Save one filtered player screenshot per key press, restore the game's interface and the overlay afterwards, and
  never trigger the game's own unfiltered screenshot as well.

## Automated tests

All in [`tools/report_check`](../../tools/report_check/README.md); build and run as described there (x86 MSVC prompt,
Python 3, the static x86 vcpkg ImGui). They use temporary files and an ImGui context; they do not run the game or
validate its GPU, lighting, loading transitions or the point selection callback.

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `report_check.cpp` | Capture storage, atomic descriptions, collections, removal and Undo, real WIC image failure and retry, the player screenshot target in an isolated game Documents `Screenshots` folder | README | None |
| `menu_253_check.cpp` | The current Report page drawn with the real Violet widgets, fonts, icons and translation tables: EN/PT/ES/FR, normal and narrow layouts, idle, recording, pending save, failure, the post-save notes form; clicks the real buttons; 24 notice cases (four languages, three widths, two font sizes: first-frame centering, full one-line text) | README (`extract_menu.py` first) | ImGui |
| `recorder_check.cpp` | Production request, cancel and deadline bodies | README | None |
| `overlay_check.cpp` | Overlay clock and the extracted loading gate against simulated state | README | None |
| `control_layout_check.cpp` | Profile picker and shared control geometry: popup stack, adjacent heights, text ink centres, EN/PT/ES/FR at two scales | README | ImGui |
| `menu_check.cpp` | Superseded staged redesign; historical only | - | - |

Native PNG previews use CPU rasterization and real WIC output; they do not prove DX9 driver behaviour or performance.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-02 | `2.5.4-rc-display-report253` | Unified Release x86 build | Passed without reported warnings or errors | MSBuild |
| 2026-10-02 | `2.5.4-rc-display-report253` | `menu_253_check` | 40 native UI cases passed across EN/PT/ES/FR, two widths and font scales (idle, recording, pending save, failure and retry, notes form); existing descriptions preserved; unnamed captures and completed sessions got factual metadata | None |
| 2026-10-02 | `2.5.4-rc-display-report253` | `report_check` | 52 checks passed, including screenshot completion, failure and retry, file-operation protections | None |
| 2026-10-02 | `2.5.4-test-report-library` | `report_check` | 52 checks passed (UTF-8 descriptions, required body validation, atomic edit preservation, collisions and Undo, collections, real WIC failure and retry) | None |
| 2026-10-02 | `2.5.4-test-report-library` | `recorder_check` | 10 request, cancel and auto-save scenarios passed | None |
| 2026-10-02 | `2.5.4-test-report-library` | `overlay_check` | 15 clock and startup-gate checks passed (ImGui averaging after 70 s without a drawn menu, genuine 500 ms frames kept, simulated load and unload) | None |
| 2026-10-02 | `2.5.4-test-report-library` | `menu_check` | 312 frames in four languages and two sizes with no missing translations or ImGui assertions; 18 interaction checks; 120 notice regression cases | None |
| 2026-10-02 | 2.5.4 hotfix | One-click capture and translation checks | Passed | None |

## In-game test plan

1. Load a world, open Report a problem, Save a report. **Expected:** a notice, a new `... Report` folder with log,
   settings and `About this capture.txt`, and the "Capture saved" form.
2. In the form, try Save capture with a blank title. **Expected:** disabled. Enter a title and save. **Expected:**
   `User notes.txt` starts with `Title: <title>`; the list shows the title.
3. Save another capture and press Cancel in the form. **Expected:** that new folder is deleted; older captures stay.
4. Start a session, take a recording, a light capture and a snapshot, end the session. **Expected:** one `... Session`
   folder with `HH-MM-SS <kind>` subfolders and `About this session.txt`; no form while the session was open.
5. Delete a capture (two clicks within 4 s). **Expected:** it disappears; the open session cannot be deleted.
6. Press the screenshot key in game. **Expected:** the interface disappears for one frame, "Screenshot saved", and a PNG
   in `Documents\Electronic Arts\The Sims 3\Screenshots` with Apex effects and without the game interface; the game
   does not save a second, unfiltered shot.
7. Turn "Hide game UI in screenshots" off and repeat. **Expected:** the interface is in the PNG.
8. Press F10 before taking a screenshot (interface hidden). **Expected:** it stays hidden afterwards.
9. With the menu open, save a capture with Include a screenshot on. **Expected:** `Screenshot.png` without the menu.

## Confirmed in game

- 2.5.0 to 2.5.5: the Report page, sessions, captures and their screenshots were used to report and diagnose lighting
  problems.

## Open checks

- Player screenshot in game: message-pump timing of the posted F10, the restored interface state, CC and game visuals,
  and the PNG output.
- Required title, Cancel deletion and the notes form across languages and text sizes in game.
- Loading gates, all capture, recording and cancel flows, GPU screenshots and the earlier reported slowdown, with the
  layouts restored after the staged test.

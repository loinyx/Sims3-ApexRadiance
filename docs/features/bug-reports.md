# Report a problem and screenshots

The **Report a problem** page gathers what a player needs to send a useful bug report: a report (log and settings), a
few seconds of lighting recording, a light capture at one spot, and a lighting snapshot. Each capture goes into its own
dated folder that is never overwritten, can carry a screenshot, and gets a title and optional description after saving.
A capture session groups several captures into one folder. The page lists saved captures with Open and Delete. A
separate screenshot shortcut saves one filtered PNG, with all Apex effects, to the game's own Screenshots folder.

## Status

| | |
|---|---|
| Availability | Report a problem: Released in 2.5.0. Post-save title and description: Released in 2.5.5 (title optional); required title and Cancel that deletes the new capture: In development (PR #2). Filtered player screenshots: In development (PR #2) |
| Default | Capture screenshots on (`[ui] capture_screenshot`); screenshot shortcut on, key `F8` (the game's own C is untouched), saved to the game's Screenshots folder, game UI hidden |
| Menu | System > Report a problem. Settings > Menu > Screenshot capture. Settings > Shortcuts > Report a problem |
| Configuration | `[ui] capture_screenshot`, `screenshot_folder`, `screenshot_key`, `screenshot_hide_game_ui`, `recorder_key`, `probe_key`, `diagnostics_key` (see [ui.md](../ui.md#shortcuts)) |
| Source | [`features/captures.h`](../../features/captures.h), [`features/captures.cpp`](../../features/captures.cpp), [`apex_gui.cpp`](../../apex_gui.cpp) (`ReportPage`, `CaptureNote`, `ScreenshotCaptureCard`) |

## The problem

A lighting bug described in words ("the room goes dark") is rarely reproducible. The evidence that explains it, the
log, the settings, what the lighting modules decided and what the GPU drew, lives in files and internal state that
players cannot find. The game's own screenshot key also saves the image without Apex Radiance's post-processing, so it
does not show what the player saw.

## How Apex Radiance solves it

The developer instruments that explain lighting problems are available to every player, renamed and explained in plain
language:

| Player name | Tool | Shortcut action (Letters / Numbers / F keys) |
|---|---|---|
| Recording | [Recorder](dev-tools/recorder.md) | Recorder (`X` / `6` / `F6`) |
| Light capture | [Light Probe](dev-tools/light-probe.md) | Probe (`V` / `4` / `F7`) |
| Lighting snapshot | [Light Diag](dev-tools/light-diag.md) | Diagnostics (`B` / `5` / `F8`) |
| Report | Log, settings and crash file only | None |

Every capture is complete on its own: log, settings, crash file and an explanation of how to zip and send it. While a
recording runs, the lighting modules write their detailed log lines even in normal mode (`Recorder::Verbose()`); only
log lines change, no lighting behaviour depends on them.

The screenshot shortcut reads the game's final back buffer after Ambient Occlusion, Edge Smoothing, Depth Blur and
Picture have rendered, with the Apex menu and notices suppressed and, by default, the game's interface hidden for one
frame.

## Settings

### Report a problem page (in order)

| Card | Controls |
|---|---|
| Capture session | Bug icon, three numbered steps, "Start a session". While open: elapsed time and count, the last 6 captures with check marks ("... and N more"), "End and save the session", "Open its folder" |
| Save a capture | Save a report (Save); Record a few seconds (Start / Stop, shows its key); Capture the light at a spot (key chip only: close the menu, point and press the key); Lighting snapshot (Save, shows its key); note "The recording and the two lighting captures need Night Lights on" when Night Lights is off; switch "Include a screenshot" |
| Your captures | Newest first, count and total size; each row shows the title (or date, time and kind), size, Open and Delete; "Open the captures folder"; "Delete all" |
| How to report a problem | Three steps (make it happen, save a capture, zip the folder and send it on Nexus Mods or GitHub), a Compare tip with its key, and a warning when `ApexRadiance_Crash.txt` is less than 7 days old |

Buttons are disabled while the game is loading, while files are being saved, while a light capture is running or while
a recording runs (except Stop). After a failed save the page shows "Some files could not be saved" and "Retry saving".

Delete asks for a second click within 4 seconds ("Click again to delete") and deletes permanently; "Delete all" works
the same way. The open session cannot be deleted. The list is read again every 2 seconds while the page is open.

### Capture saved form

After a capture finishes (and no session or recording is running), a 500-unit-wide modal "Capture saved" ("Give it a
name to find it more easily") asks for **Title (required)** and **Description (optional)**.

| Button | Effect |
|---|---|
| Cancel | Deletes the new capture folder (`Captures::Delete`) and closes the form; on failure shows "Could not delete the capture. Check folder access and try again." |
| Save capture | Enabled once the title has a non-blank character. Writes `User notes.txt` (`Title: <title>`, a blank line, then the text); a blank description keeps the automatic text |

Captures saved before the page was opened do not trigger the form.

### Screenshot settings

| Control | TOML (`[ui]`) | Default | Notes |
|---|---|---|---|
| Include a screenshot (Report page) | `capture_screenshot` | true | Adds `Screenshot.png` to every capture |
| Save screenshots to | `screenshot_folder` | `game` | `game`: Documents > Electronic Arts > The Sims 3 > Screenshots; `apex`: the Apex Radiance folder > Screenshots |
| Screenshot key | `screenshot_key` | `F8` | Custom only, not part of the presets; a saved bare letter or digit (e.g. `C`) falls back to the default with a log line |
| Hide game UI in screenshots | `screenshot_hide_game_ui` | true | Hides the game's interface for the shot, then restores it |

## Compatibility and interactions

- Bare F10 always reaches the game (its interface toggle); Compare uses its own key, not bare F10.
- Hiding the game UI posts F10 with `Overlay::PostGameKeyPress`, which bypasses Apex shortcut handling. Apex tracks F10
  presses it sees after its window hook starts; it cannot know an interface state toggled before that.
- The screenshot is the back buffer, not the game's `SceneCaptureManager` photo path, whose handling of
  post-processing is not established.
- Shortcuts do not fire while the game's cheat console is open (Ctrl+Shift+C until Enter or Esc); the bare screenshot
  key passes through while a menu text field is active.
- Captures need a loaded world: the menu and its shortcuts become available only after the world is loaded and settled
  ([ui.md](../ui.md#startup-and-notices)).

## Limitations

- Screenshots are read only from 8-bit `X8R8G8B8` / `A8R8G8B8` back buffers (multisampled ones are resolved first);
  otherwise the capture has no `Screenshot.png` and the log says "No screenshot: the screen format could not be read".
- With the menu open, a capture's screenshot is taken before the menu draws.
- If the game does not process the posted F10 within 2 seconds, the player screenshot is cancelled.
- Captures may contain local paths and session details; nothing is uploaded automatically. Zipping and sending is
  manual.
- The light capture and the lighting snapshot need Night Lights on; the recording saves what is available without it.

## Technical reference

### Capture folders (`features/captures.cpp`)

Root: `Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\`.

| Capture | Folder | Main file |
|---|---|---|
| Report | `YYYY-MM-DD HH-MM-SS Report\` | Only the copies below |
| Recording | `... Recording\` | `Recording.txt`, `Wall seams.csv` |
| Light capture | `... Light capture\`; automatic follow-ups after a floor change: `... Light capture (automatic)\` | `Light capture.txt`, textures and shaders |
| Lighting snapshot | `... Lighting snapshot\` | `Lighting snapshot.txt` |
| Session | `... Session\` holding `HH-MM-SS <kind>\` folders | `About this session.txt` (the list) |

- `NewFolder` / `MakeFolder`: a taken name gets " (2)", " (3)" and so on; a folder is never reused.
- `Finish` copies `ApexRadiance_LOG.txt`, `ApexRadiance.toml` and `ApexRadiance_Crash.txt` when present, writes
  `About this capture.txt` (version, game build, time, what it is, how to zip and send it) and `User notes.txt` when a
  title or description exists, then shows the saved notice. A session gets the same copies when it ends.
- Nothing is deleted or overwritten by itself. `Delete` removes only direct children of `Captures\` (never the open
  session, never while saving) and logs `[Captures] Deleted from the menu: Captures\<folder>`. `DeleteAll` skips the
  open session and `.Removed`.
- Reversible removal (`.Removed` and Undo) remains in the API but the menu uses Delete; `.Removed` folders are excluded
  from the list.
- Text that fails to write is retained for `RetrySave` without repeating the measurement; a failed screenshot is taken
  again from the current frame.
- Opening folders runs Explorer on a short-lived thread with COM (`ShowInExplorer`), never inside the menu frame.

### Capture screenshots

`Finish` queues `Screenshot.png` when `[ui] capture_screenshot` is on. The next frame is read:

- Menu closed: at Present (`D3D9Hooks::Priority::First`, hook `CapturesScreenshot`), the frame as shown with Picture
  included; capture notices are not drawn that frame (`ScreenshotPending`).
- Menu open: at `RenderCallbacks::filteredSceneBeforeOverlay`, after Picture and before the Apex menu.

Back buffer -> resolve if multisampled -> `GetRenderTargetData` into SYSTEMMEM -> BGR 24-bit (alpha dropped: it is the
bloom mask) -> PNG with WIC on a short-lived thread. The save receipt waits for the PNG.

### Player screenshot (`RequestPlayerScreenshot`)

1. Dispatched from `RunShortcuts` on the `Screenshot` action.
2. File: `<GameDocuments>\Screenshots\Screenshot_YYYY-MM-DD_HH-MM-SS_mmm.png` (" (2)" on collision). Folder failures
   notify "Could not create the screenshots folder".
3. `QueuePlayerPhoto`: hides the overlay, suppresses capture notices, posts F10 when hiding the UI and it is not already
   hidden (failure: "Could not hide the game interface; screenshot was not taken"), queues the shot with one Present
   skipped. Log `[Captures] Filtered screenshot requested: <file>`.
4. `TakeShots` waits until the posted F10 is observed (`ObserveGameUiKey`); after 2 seconds it logs
   `[Captures] Screenshot cancelled: the UI hide key was not processed in time` and restores.
5. After writing, `RestorePlayerPhoto` posts F10 again if it hid the UI and restores the overlay. Notice
   "Screenshot saved" (Camera icon) or "The screenshot could not be saved" (warning).

### Notices

Each capture start and completion shows a top-center notice (`Captures::Notify`, kinds Info, Success, Warning, Saving,
Screenshot, Probe): for example "Recording saved. Open Report a problem to find your files", "Light capture saved. Open
Report a problem to find your files", "Lighting snapshot saved. Open Report a problem to find your files". Layout and
priority are in [ui.md](../ui.md#startup-and-notices).

### Files and functions

| File | Symbol | Role |
|---|---|---|
| `features/captures.h/.cpp` | `Root`, `NewFolder`, `Finish`, `WriteText`, `SaveReport`, `RecentCrash` | Folders and completion |
| | `BeginSession`, `EndSession`, `SessionActive`, `SessionItems`, `SessionFolder` | Sessions |
| | `List`, `Open`, `OpenFolder`, `Delete`, `DeleteAll` | Library |
| | `ReadDescription`, `SaveFolderDescription`, `LastSave`, `RetrySave` | Notes and receipts |
| | `QueueShot`, `TakeShots`, `RequestPlayerScreenshot`, `ObserveGameUiKey` | Screenshots |
| | `Notify`, `CurrentNote` | Notices |
| `apex_gui.cpp` | `ReportPage`, `SessionHeroCard`, `ReportCaptureCard`, `ReportListCard`, `ReportHowCard`, `ReportOptionalNotes`, `ScreenshotCaptureCard`, `RunShortcuts` | UI |
| `framework/overlay.cpp` | `PostGameKeyPress`, `SetCaptureSuppressed` | Key passthrough and notice suppression |

## Rejected approaches

- Guided problem choices and a one-click target as the page's first step
  ([history](../history/bug-reports.md#2026-10-02-guided-capture-preview)).
- A four-stage workspace with a required description
  ([history](../history/bug-reports.md#2026-10-02-visible-stages-and-saved-files-test)).
- Reversible removal into `.Removed` with Undo in the menu
  ([history](../history/bug-reports.md#2026-10-02-restored-capture-page)).
- Opening Explorer from the menu frame
  ([history](../history/bug-reports.md#2026-09-30-explorer-crash-and-layout)).

## See also

- [Validation](../validation/bug-reports.md)
- [History](../history/bug-reports.md)
- [Menu reference](../ui.md), [Recorder](dev-tools/recorder.md), [Light Probe](dev-tools/light-probe.md),
  [Light Diag](dev-tools/light-diag.md)

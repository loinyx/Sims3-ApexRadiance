# Report a problem: captures for bug reports

> Since 30/09 (after 2.4.0) the lighting recorder (F6), the light probe (F7) and the light diagnostics (F8), which were
> development-build tools, are in both builds, renamed for players, and gathered on one menu page, **System › Report a
> problem**, with plain explanations. Every capture goes into its own dated folder that is never overwritten; capture
> sessions gather several captures in one folder; the page lists the saved captures with Open and Delete. Code:
> `features/captures.{h,cpp}` (folders, notes, sessions, list), `apex_gui.cpp` (`ReportPage`, `CaptureNote`).

## Published since 2.5.5: restored capture page

**Follow-up user request:** retain the restored page and offer optional title/description after saving. A small post-save form has Title (optional), Description (optional), Keep automatic details, and rightmost primary Save capture. Blank fields keep automatic diagnostic metadata. Dismissing the form never cancels the already-saved capture. During a collection the prompt waits until the collection is finished. Older captures do not trigger prompts when the page opens. Names and notes live in `User notes.txt`; dated directories are not renamed, so pending screenshots, session entries and retry paths remain valid. The existing compact list shows a custom title in its existing label position. Generic details record the capture type, version and game information, never speculate about the user's problem. New standalone captures and completed collections get fallback metadata even if the form is never opened. Existing notes remain intact.

At the user's request, the Report page and its capture library use the layout from the actual `v2.5.3` tag again: Capture session, Save a capture, Your captures, and How to report a problem. Capture controls are always present. The Capture/Saved files tabs, guided stages, compulsory title/description editor, completion receipt, and informational dialogs from today's redesign are removed. Developer capture tools reuse the restored session/capture/library helpers.

This was a scoped GUI restoration, not a whole-file checkout. Developer mode, lighting fixes, loading gates, centered notices, screenshot threading and capture storage are retained. Display/fluency was subsequently removed before publication. Existing User notes.txt files are preserved. No saved capture is deleted during migration. The v2.5.3 two-click Delete/Delete all actions are restored (permanent deletion when explicitly confirmed); `.Removed` folders from earlier tests remain excluded. Busy guards prevent deleting files or starting/ending sessions while capture output is pending. A failed save retains Retry saving.

The old redesign fixture tests superseded state fields. Use `tools/report_check/menu_253_check.cpp` for the restored page; backend report/recorder checks remain applicable. Native checks and compilation do not replace gameplay validation. Nothing is installed or published automatically.

Validation for `2.5.4-rc-display-report253`: unified Release x86 compilation passed without reported warnings/errors; 40 native UI cases passed across EN/PT/ES/FR and two widths/font scales, covering idle, recording, pending save, failure/retry and the optional notes form. Existing descriptions were preserved, and unnamed standalone captures and completed sessions received factual automatic metadata. The existing real-storage suite passed 52 checks, including screenshot completion, failure/retry and file-operation protections. Display/developer shared action helpers were retained after the dependency audit. Gameplay capture flows still need validation; no installation or publication occurred.

## Superseded local test: visible stages and saved files

**Version `2.5.4-test-report-library`, not published or installed automatically.** Implements the approved “Visible stages” prototype (option 2), building on the previous inline-description test. The description modal remains removed. The released 2.5.4 keeps its earlier layout; historical sections below describe its development. The same Report page is compiled in player and private builds; Developer remains private.

### One workspace, four visible stages

The stage strip shows **Prepare → Record → Describe → Completed**. It reflects progress without adding navigation or allowing stages to be skipped. It wraps to two columns for narrow panels or large text.

1. **Prepare:** keep the problem visible in the game, then Start recording. Capture options are hidden until recording begins. Starting keeps the panel open. The recording collects diagnostic data, not video, for up to 20 seconds.
2. **Record:** one card holds progress, point capture, lighting snapshot, general report, screenshot preference and collapsed collection/comparison options. These actions wait only for a pending capture/write or debounce, not for the whole recording. **Return to game** optionally closes the panel; reopen it while recording to access the same controls. Point selection temporarily hides it and returns after one click and capture completion. Comparison keeps the panel open, and Restore Apex effects remains available after recording and on Saved files. Point/snapshot measurements require Night Lights; recording still saves available diagnostics when those effects are off. **Stop and continue** requests a stop and proceeds to details after writes finish; it does not claim the report is complete yet. Cancel recording remains idempotent and creates no recording folder; it does not remove any extra captures already saved.
3. **Describe:** the recording workspace is replaced by saving status, then the inline **Problem details** card. Title is optional; description is required. **Cancel** and **Save capture** align right, Save last, stacking on narrow panels. Blank/whitespace descriptions cannot complete the report. Cancel preserves captured files and leaves the report pending; cancelling an edit preserves the previously saved description. Failed atomic note replacement preserves the old file and keeps the draft for retry. Pending or failed receipts remain at this stage.
4. **Completed:** successful files plus a saved description show the receipt, folder and sharing guidance. Nothing is uploaded automatically. An open collection must still be finished before How to send becomes available.

After saving the description, its card is replaced by a single receipt card: expandable Your description, What was saved and How to send sections, with New recording and Open its folder actions aligned right. How to send requires a saved description and a finished collection. New recording returns to the initial card. An unfinished collection can be finished from the receipt. UTF-8 `User notes.txt` lives at the direct capture/collection root; diagnostic measurements are not rewritten by editing it. Captures created with shortcuts also use the details state when the page is next opened.

### Saved files

Capture / Saved files remain horizontal tabs. Saved folders are newest first with size, grouped capture count and a visible reminder when a description is missing. Open and Contents use the real files. Add or edit description uses the same replacement card. How to send is disabled without a description or while a collection is open. Refresh scans run at most every two seconds while the library is visible; the default Capture page does not enumerate the library. Paths, filenames and user notes are not translated.

Remove and Remove all ask for confirmation, then move folders into `Captures/.Removed` rather than permanently deleting them. Undo restores the last successful removal/batch in this game session; name collisions never overwrite either folder. Active writes and the open collection are protected. The recovery folder remains on disk across game restarts and is excluded from the library and totals.

### Startup availability and performance diagnosis

All Apex panel opening, notices and diagnostic shortcuts are withheld until a loaded game session is active and remains ready for three seconds. With Night Lights enabled this additionally requires its existing world-terrain-drawn/world-live signal; the first loading screen and main menu cannot open the panel. Returning to an inactive/unloaded session closes it. With lighting off/refused the fallback reads the documented WorldManager active flag (+0x41) and loaded/edit/save modes (+0x1B4) through the resolved global. This is read-only, checked every 200 ms on the render thread; the window thread sees only the cached atomic result. Actual EA/Steam loading transitions and the lighting-off fallback still need gameplay validation.

The overlay clock now samples every game frame, including frames where it draws nothing. Reopening after a long closed interval does not feed that whole interval into ImGui's FPS average or animation delta. This fixes a counter error; it does not claim to fix a genuine low-FPS frame. Real stalls are not clamped away. A slow visible panel logs lock wait, backend preparation, UI build, DX9 submission and game-frame delta at most once per ten seconds, under `[Overlay] Slow panel`. The trace does not measure GPU execution time or work elsewhere in the game.

The user subsequently reported smooth operation again after a separate system/GPU-memory investigation. The exact cause of the earlier system-wide slowdown was **not established**, and this UI revision does not claim a performance fix. One earlier Picture summary recorded 117 passes in a minute, consistent with approximately 2 FPS, so it cannot be dismissed as only the averaging error. The point-in-time system sample had ample free physical memory and no aggregate CPU saturation; that does not rule out transient CPU, driver or GPU stalls. No graphics DLL, game settings or lighting solver was changed for this UI revision.

### Sharing, selection and failures

How to send explains creating a ZIP manually in Explorer and attaching it to a Nexus Mods bug report or GitHub issue. Review images, description, log and settings before sharing. Nothing is uploaded automatically. Opening/managing files in Explorer remains possible while a report is pending.

Point selection keeps its centered target, disappears after one click and returns to Report after probe and PNG completion. Esc, focus loss or reopening the menu cancel aiming. Startup/capture/recording/comparison pills use the existing top-center anchor and icon pack. Other Report dialogs (contents/share/removal) retain right-aligned actions. Page defaults restore the screenshot preference, not recordings, notes or files.

Main diagnostic text and metadata writes are retained for explicit retry; PNG completion is acknowledged by the WIC worker, not assumed on queueing. Retry does not repeat the measurement. A failed image retry takes the **current frame again**. A failed collection manifest keeps the collection open and does not double-count children. Only the latest receipt owns retry; retained text is not durable across game exit. Auxiliary probe shader/texture diagnostics are preserved but are not regenerated by this retry. Explorer remains on its separate COM thread.

### Validation

The storage harness passed 52 checks, including UTF-8 descriptions, required body validation, atomic edit preservation, collisions/Undo, collections and real WIC failure/retry. Ten production recording request/cancel/auto-save scenarios passed. Fifteen clock/startup-gate checks exercise real ImGui averaging after 70 seconds without a drawn menu, preservation of genuine 500 ms frames and simulated load/unload states; they access no real game memory or files.

The current native Report code rendered 312 frames in EN/PT/ES/FR at normal/narrow sizes, with no missing translations or ImGui assertions. Assertions verify options are hidden before recording, shown during recording, and replaced by inline details, with no description popup. Eighteen native interaction checks click the actual Violet buttons and exercise panel visibility at start/comparison, optional return, point aiming, recording cancellation, stop-to-details, restoring effects after recording, blank/whitespace validation, pending cancellation, saving, preserving a cancelled edit returning to preparation, and expanding saved contents and sharing guidance without informational popups. An additional 120 native notice regression cases cover four languages, three viewport widths and two font sizes, including recovery from a previously narrow window. Recording/probe game APIs are inert in this UI fixture; the description files are real temporary files. CPU PNG rendering checks spacing; it does not validate the DX9 driver or GPU performance. Gameplay validation remains required for loading gates, all capture/recording/cancel flows, languages/scales, GPU screenshots and the reported slowdown.

### Grouping and compatibility

Additional captures retain their existing storage behavior: each has its own dated folder unless the user starts an optional collection. A collection groups subsequent recording/point/snapshot/report files under its root; it is not started automatically. No ZIP is generated automatically, no files are sent, and no lighting solver or game-memory hook changes in this redesign. Public and private test builds share this page; only the private build contains Developer tools.

## Files

`Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\`:

| Capture | Folder | Main file |
|---|---|---|
| Report | `YYYY-MM-DD HH-MM-SS Report\` | (only the copies below) |
| Recording | `... Recording\` | `Recording.txt` (as before: the lines sorted by clock time, the toml at the start) |
| Light capture | `... Light capture\` (the automatic follow-ups after a floor change: `... Light capture (automatic)\`) | `Light capture.txt` + the textures (BMP) and shaders |
| Lighting snapshot | `... Lighting snapshot\` | `Lighting snapshot.txt` |
| Session | `... Session\` holding `HH-MM-SS <kind>\` folders | `About this session.txt` (the list) |

Every capture folder also gets, when it is complete (`Captures::Finish`): a copy of `ApexRadiance_LOG.txt`,
`ApexRadiance.toml`, `ApexRadiance_Crash.txt` when present, and `About this capture.txt` (version, game build, time, what
it is, how to zip and send it). A session gets the same copies when it ends. A name already taken gets " (2)", " (3)"...
Nothing is deleted or overwritten by itself (the light probe's old 20-capture pruning and the "latest" copies
`ApexRadiance_LightProbe.txt` / `ApexRadiance_LightDiag.txt` are gone). Delete removes only direct children of
`Captures\`, never the open session.

## Detail in the public build

The recording reads detailed log lines that the lighting modules used to write in the development build only (the solve
journal of `level_light_share.cpp`, the lot lamp change details of `lot_light_bridge.cpp`, the lamp change decisions of
`night_terrain_relight_patch.cpp`, the furniture tracer). They now test `Recorder::Verbose()` (= development build, or a
recording running), so a player's recording has them too and the public log stays quiet otherwise. Only log lines changed:
no lighting behaviour depends on them.

## Shortcuts

`hotkeys.cpp`: the public build now takes Recorder, Probe and Diagnostics (Frame Capture stays development-only). Action
names for players: "Recording", "Light capture", "Lighting snapshot".

## Historical revision (30/09, after the first test)

- **Crash fixed:** Open / Open the captures folder called ShellExecuteW inside the menu frame; it pumped the game window's
  messages, the overlay's window procedure ran again inside the frame and its std::mutex threw (resource deadlock,
  crash report 22:10:22, `Overlay::ApexWndProc` -> `std::_Throw_Cpp_error`). Explorer is now opened on a short-lived
  thread with COM (`ShowInExplorer`), like the Profiles folder button. Never call ShellExecute from the menu frame.
- **Layout (user: sessions higher, clearer, a nicer look):** the page is now Session (a violet-edged card of its own:
  big icon, three numbered steps and "Start a session"; while open, a pulsing dot with its time and count, the captures
  so far with check marks, "End and save the session" and "Open its folder") -> Save a capture -> Your captures -> How
  to report a problem.
- **Screenshots:** every capture also gets `Screenshot.png` (switch "Include a screenshot", `[ui] capture_screenshot`,
  default on). Taken on the next frame: menu closed = at Present (the picture as shown, Color filters included; the
  capture notes are not drawn that frame), menu open = at `endSceneBeforeOverlay` (before the Apex menu; the Color pass
  comes after the menu, so it is not in that one). Back buffer -> SYSTEMMEM (GetRenderTargetData, a resolve first if it
  were multisampled), BGR 24-bit, encoded with WIC on a short-lived thread. 8-bit back buffers only.

## Player screenshots

For new or missing screenshot-key settings, Settings > Shortcuts enables the player screenshot shortcut on C,
replacing the game's native screenshot key while the option is enabled. Apex consumes that key and writes one filtered PNG to the game's standard Documents
`Screenshots` folder; it does not also invoke the game's unfiltered screenshot. Existing saved screenshot keys remain
unchanged. Bare F10 remains available for the game's UI toggle, which the mod uses internally only while hiding the
interface for the shot. Apex only runs Compare on the exact Ctrl+Shift+F10 chord; the synthetic bare F10 bypasses Apex
shortcut interception.
It reads the game's final back buffer at Present, after Ambient Occlusion, Edge Smoothing, Depth Blur and Picture have
rendered, and writes a timestamped PNG to the game's standard `Screenshots` folder in Documents. This differs from `SceneCaptureManager`'s
off-screen photo/thumbnail path, whose inclusion of post-processing is not established. Apex's overlay is suppressed
for the shot. By default, the mod sends F10 for a single frame to hide the game's UI, captures the frame, then restores
the prior tracked F10 state. It tracks F10 key presses seen after the overlay hook starts; it cannot infer an earlier
UI state if F10 was pressed before that hook was installed. The player can disable UI hiding or rebind the screenshot
to a bare key or modifier chord in Settings. Compile and offline checks cannot validate the game message-pump timing,
F10 behavior, CC/game visuals, or screenshot output in a running game; test those in game before release.

## Guided capture (local preview, 2026-10-02)
The public and private Report page now starts with four plain-language choices: lighting, an object's appearance, a crash, or another/unknown problem. Selecting one starts or reuses a capture session. The guide presents only the relevant action; all existing capture tools, session controls, help and the capture library remain available in collapsed sections. Finishing explicitly ends the session and opens its folder. ZIP creation remains manual; no automatic upload is performed.
The appearance action arms a one-shot light probe and closes the menu. A Violet target follows the mouse and the top-left capture note displays the actual configured probe chord and Esc cancellation. Opening the menu also cancels selection. The marker is visual only, does not identify an object, and does not run scene searches. A guided capture does not schedule automatic floor-change follow-ups; the direct shortcut retains that diagnostic behavior. No target is drawn in capture screenshots. Selection is transient and not saved to TOML. The guide, hint and controls are translated into EN/PT/ES/FR. Gameplay validation of pointer alignment and capture completion remains necessary.

## One-click selection and centered notices (private, 2026-10-02)
This supersedes the guided selection instructions above. A left click while the target is active confirms that client-area pixel, hides the target and instruction immediately, and consumes both mouse down and mouse up so the game does not also select or place an object. The clicked coordinates are queued atomically; only the render thread starts the GPU probe. The existing probe shortcut also confirms selection. Esc, losing focus or reopening the menu cancels selection. This selects a screen pixel, not an object identity.
After a confirmed selection, the Report page reopens once the probe and pending screenshot are complete. It does not open over the captured frame. An idle open capture session no longer keeps a permanent on-screen chip; the session remains available on the Report page. Capture/save acknowledgments remain transient.
Startup, capture, recording and comparison pills share one top-center anchor, 20 scaled pixels below the viewport top, with consistent padding, rounding, background and border. Only one routine pill is displayed there at a time: capture/recording takes precedence over comparison, which takes precedence over the startup hint. Compatibility warnings and first-start interactive notes use the same anchor and suppress routine pills. Captures suppress routine notices and the target in their screenshot. Updated location and click instructions are translated into EN/PT/ES/FR. Build checks do not replace an in-game click/cancel and scaling test.

### 2.5.4 public hotfix scope

The shared Report changes above, page defaults and centered notices are included in the public build. Developer-only diagnostics and profiler controls are excluded. The one-click capture regression checks and English/Portuguese/Spanish/French translation checks passed. The separate simpler recording/saving design remains a prototype; no automatic ZIP generation or upload was added.

### Local test results

See the current local-test validation section above and tools/report_check/README.md. The description modal revision was superseded by the inline workflow; its old previews/build files are not this candidate.

## Local completion review update
The completed receipt keeps user title, folder and right-aligned New recording/Open its folder actions in one card. Your description, What was saved and How to send are expandable sections. Capture contents are scanned only on first expansion per receipt; note text is cached from its initial read or explicit edit, not reread every frame. Sharing remains disabled until required notes and collection completion are satisfied. Library informational views use inline cards; only removal keeps a confirmation popup. The editor remains an inline replacement. Central text pills set content-measured width before Begin to prevent the narrow vertical-strip feedback between wrapping and auto-sizing; they use Lucide icons and the entry logo. All previous lifetime/input/startup behavior remains.

### Saved capture library redesign (local test, 2026-10-02)

Each entry leads with the user title and full description, followed by date/time. Untitled older captures use translated plain names by capture type; missing descriptions have an explicit prompt. Open its folder and Add or edit description remain visible. Files and more actions expands the technical folder name, size, actual contents, sending guidance and confirmed removal. Bulk removal is under Manage saved captures; Undo remains available there. All new labels support EN/PT/ES/FR. Notes are returned by the existing bounded ReadDescription during the two-second visible-library scan, rather than adding per-frame disk reads or an extra read per entry. Names on disk and stored notes remain unchanged. The native fixture checks stored title/body visibility data and collapsed destructive/technical controls in all languages and both sizes. Gameplay validation remains pending.

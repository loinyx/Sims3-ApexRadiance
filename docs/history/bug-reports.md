# Report a problem and screenshots: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/bug-reports.md](../features/bug-reports.md).

### 2026-09-30: Report a problem page

**Context:** after 2.4.0, the lighting recorder (F6), the light probe (F7) and the light diagnostics (F8) were
development-build tools that wrote fixed files (`ApexRadiance_LightProbe.txt`, `ApexRadiance_LightDiag.txt`, the
probe's 20-capture pruning).

**Finding:** players could not use them, and fixed files were overwritten by the next capture.

**Outcome:** the three tools moved into both builds, renamed for players ("Recording", "Light capture", "Lighting
snapshot") and gathered on **System > Report a problem** with plain explanations (commit bc5e42d). Every capture gets
its own dated folder; sessions group captures; the page lists captures with Open and Delete. `hotkeys.cpp` gave the
public build the Recorder, Probe and Diagnostics actions (Frame Capture stayed development-only). The lighting modules'
detailed log lines began testing `Recorder::Verbose()` (the solve journal of `level_light_share.cpp`, the lot lamp change
details of `lot_light_bridge.cpp`, the lamp change decisions of `night_terrain_relight_patch.cpp`, the furniture
tracer). The "latest" copies and the 20-capture pruning were removed.

### 2026-09-30: Explorer crash and layout

**Context:** the first test of the page (crash report 22:10:22).

**Finding:** Open and "Open the captures folder" called `ShellExecuteW` inside the menu frame; it pumped the game
window's messages, the overlay's window procedure ran again inside the frame and its `std::mutex` threw (resource
deadlock, `Overlay::ApexWndProc` -> `std::_Throw_Cpp_error`). Maintainer feedback also asked for sessions higher on the
page, clearer, with a more distinct look, and for a picture of the screen with every capture.

**Outcome:** Explorer opens on a short-lived thread with COM (`ShowInExplorer`), like the Profiles folder button; never
call ShellExecute from the menu frame. The page order became Session (a violet-edged card with a large icon, three
numbered steps and "Start a session"; while open a pulsing dot with time and count, the captures so far, "End and save
the session" and "Open its folder") -> Save a capture -> Your captures -> How to report a problem. Every capture got
`Screenshot.png` (commit 8b71e9a).

### 2026-10-02: guided capture preview

**Context:** a local preview started the page with four plain-language choices: lighting, an object's appearance, a
crash, or another/unknown problem. Choosing one started or reused a session and showed only the relevant action, with
the other tools, session controls, help and library collapsed. Finishing ended the session and opened its folder.

**Finding:** the appearance action armed a one-shot light probe and closed the menu; a violet target followed the mouse
and the capture note showed the probe key and Esc. The marker was visual only, did not identify an object and did not
run scene searches. Guided captures did not schedule floor-change follow-ups. The target was never drawn in screenshots,
was not saved to TOML and was translated into EN/PT/ES/FR.

**Outcome:** superseded by one-click selection the same day, then by the restored page.

### 2026-10-02: one-click selection and centered notices

**Context:** the guided selection needed a clearer confirm step; notices were drawn at different positions.

**Finding:** a left click while the target is active confirms that client-area pixel, hides the target and instruction
immediately and consumes both mouse down and mouse up, so the game does not also select or place an object. The
coordinates are queued atomically and only the render thread starts the GPU probe; the probe key also confirms; Esc,
focus loss or reopening the menu cancels. It selects a pixel, not an object. After a confirmed selection the Report
page reopened once the probe and screenshot were complete. An idle open session no longer kept a permanent chip.

**Outcome:** startup, capture, recording and comparison pills share one top-center anchor 20 scaled pixels below the
top, with one routine pill at a time (capture/recording, then comparison, then the startup hint); compatibility warnings
and first-start notes use the same anchor and suppress routine pills; captures suppress routine notices and the target
in their screenshot. Released in the 2.5.4 public hotfix with the shared Report changes, page defaults and centered
notices; developer-only diagnostics and profiler controls were excluded. The one-click selection code (`LightProbe::Aim`,
`ConfirmAim`) remains, but no current menu control starts it.

### 2026-10-02: visible stages and saved files test

**Context:** local test `2.5.4-test-report-library`, implementing a "Visible stages" prototype (option 2) on top of an
inline-description test. Not published or installed.

**Finding:** the design had a stage strip **Prepare -> Record -> Describe -> Completed** (wrapping to two columns on
narrow panels or large text):

1. Prepare: keep the problem visible, then Start recording; capture options hidden until recording began; recording
   collected diagnostic data, not video, for up to 20 seconds.
2. Record: one card with progress, point capture, snapshot, general report, screenshot preference and collapsed
   collection and comparison options; Return to game optionally closed the panel; point selection hid it and returned
   after one click; Restore Apex effects stayed available; "Stop and continue" proceeded to details after writes
   finished; Cancel recording was idempotent and created no folder.
3. Describe: inline Problem details card, title optional, description required, Cancel and Save capture aligned right;
   failed atomic note replacement kept the old file and the draft.
4. Completed: a receipt card with expandable Your description, What was saved and How to send (manual ZIP for a Nexus
   Mods bug report or GitHub issue), New recording and Open its folder; How to send required a saved description and a
   finished collection. Capture contents were scanned only on first expansion and note text cached.

Saved files were a second horizontal tab: newest first with size, grouped count and a reminder when a description was
missing; Open and Contents; Add or edit description; refresh scans at most every 2 seconds only while visible; Remove
and Remove all moved folders to `Captures/.Removed` with Undo for the game session, never overwriting on name
collisions, with the recovery folder excluded from the library. A later library redesign led each entry with the title
and full description, translated plain names for untitled captures, "Files and more actions" for technical details and
removal, and bulk removal under "Manage saved captures", reading notes with the bounded `ReadDescription` during the
2-second scan. Retry kept text and metadata for explicit retry without repeating the measurement; PNG completion was
acknowledged by the WIC worker; a failed image retry took the current frame again; a failed collection manifest kept
the collection open without double-counting; only the latest receipt owned retry. Central text pills set their width
from content before Begin, to avoid a narrow vertical strip from wrapping and auto-size feedback.

The same test diagnosed a reported slowdown. The overlay clock began sampling every game frame, including frames where
it draws nothing, so reopening after a long closed interval no longer fed that interval into ImGui's FPS average; real
stalls are not clamped. A slow visible panel logs lock wait, backend preparation, UI build, DX9 submission and game-frame
delta at most once per ten seconds under `[Overlay] Slow panel` (no GPU execution time). One earlier Picture summary
recorded 117 passes in a minute (about 2 FPS), so the slowdown was not only the averaging error; the system sample had
ample free memory and no aggregate CPU saturation, which does not rule out transient CPU, driver or GPU stalls. Smooth
operation returned after a separate system and GPU-memory investigation; the cause was not established and no
performance fix is claimed.

**Outcome:** rejected in favour of the restored 2.5.3 layout. Validation of this test is recorded in
[validation/bug-reports.md](../validation/bug-reports.md).

### 2026-10-02: restored capture page

**Context:** the staged redesign was heavier than the original page.

**Finding:** the `v2.5.3` layout (Capture session, Save a capture, Your captures, How to report a problem) with
always-present capture controls was easier to use.

**Outcome:** a scoped GUI restoration, not a whole-file checkout: the Capture/Saved files tabs, guided stages,
compulsory description editor, completion receipt and informational dialogs were removed. Developer mode, lighting
fixes, loading gates, centered notices, screenshot threading and capture storage were kept; Developer capture tools
reuse the session, capture and library helpers. The two-click Delete and Delete all came back (permanent deletion when
confirmed); `.Removed` folders from earlier tests stay excluded; busy guards prevent deleting or starting and ending
sessions while output is pending; Retry saving stays. A small post-save form offered Title (optional), Description
(optional), Keep automatic details and Save capture; blank fields kept the automatic metadata, dismissing never
cancelled the saved capture, prompts waited until a collection finished, and older captures did not trigger prompts.
Names and notes live in `User notes.txt`; dated folders are never renamed, so pending screenshots, session entries and
retry paths stay valid. New captures and completed collections get factual fallback metadata (capture type, version,
game information) even if the form is never opened. The Display page was removed before publication. Released in 2.5.5.

### 2026-10-03: filtered player screenshots

**Context:** the game's own screenshot key saves the image without Apex post-processing.

**Finding:** the final back buffer at Present holds every Apex pass; the game's F10 can hide its interface for one
frame.

**Outcome:** a screenshot shortcut (default `C`, on for new or missing settings, existing saved keys unchanged) writes
one filtered PNG to the game's Documents `Screenshots` folder and replaces the game's native shot. Bare F10 stays with
the game; Compare runs only on its own chord, and the posted F10 bypasses Apex shortcut handling (commits fc88731,
4b20c29, 917f1d8).

### 2026-10-03: required capture title

**Context:** untitled captures were hard to tell apart in the list.

**Finding:** the description is often unnecessary, but a title always helps.

**Outcome:** the post-save form requires a non-blank title and keeps the description optional; the automatic-details
bypass was removed. The modal has a fixed responsive width and automatic height. Cancel deletes only the newly saved
folder through `Captures::Delete`, with the root, session and busy guards; a failure keeps the form open with an error.
Existing notes are never overwritten (commit 828bc1c).

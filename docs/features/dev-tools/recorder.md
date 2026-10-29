# Recorder (lighting recording)

The recorder captures up to 20 seconds of lighting activity while the player reproduces a problem. Every line carries
its clock time and the lines are sorted together: the log, the room light solves, status changes of the lighting
modules, every indoor object (furniture) whose drawing changes, every indoor room whose light changes, and Light Probe
captures. Players start it as **Record a few seconds** on the Report a problem page or with its shortcut; each
recording goes to its own capture folder, with a `Wall seams.csv` of the wall light at the floor lines.

## Status

| | |
|---|---|
| Availability | Released in 2.5.0 as a player capture (Report a problem). `Wall seams.csv`: Released in 2.6.0. Light update trace (screen pixels, room solve ends, lamp edits, per-edit summary): Released in 2.7.0 |
| Default | Idle; no switch |
| Menu | System > Report a problem > Save a capture > Record a few seconds (Start / Stop) |
| Configuration | No feature table. Shortcut `[ui] recorder_key` in `ApexRadiance.toml` (see [ui.md](../../ui.md#shortcuts)) |
| Source | [`features/recorder.cpp`](../../../features/recorder.cpp), [`features/recorder.h`](../../../features/recorder.h) |

## The problem

Indoor lighting problems often appear only for a moment: a room flashes dark after a floor switch, furniture changes
colour while a slider moves, a wall shows a seam where two stories meet. A single snapshot or probe capture misses the
sequence of events. The useful evidence is a timeline that puts the game's room solves, Apex Radiance's decisions and
what each object was drawn with side by side.

## How Apex Radiance solves it

While the recording runs, the lighting modules write their detailed lines even in normal mode (`Recorder::Verbose()` is
true in developer mode or while recording), and other modules add notes through `Recorder::Note`. At the end the
recorder merges the log written meanwhile, the solve journal and its own lines by clock time and saves them with the
settings as they were at the start.

## Settings

| Control | Where | Effect |
|---|---|---|
| Record a few seconds, Start / Stop | Report a problem > Save a capture | `Recorder::RequestToggle()`; disabled while loading, saving, probing or while another capture runs |
| Shortcut (`[ui] recorder_key`; preset `X`, `6` or `F6`) | In game | Same toggle (`Hotkeys::Take(Action::Recorder)`) |
| Include a screenshot (`[ui] capture_screenshot`, default true) | Report a problem | Adds `Screenshot.png` to the folder |

While recording, the top notice reads "Recording N s · press <key> to stop" with a red pulsing icon.

## Compatibility and interactions

- Light Probe captures taken during a recording add `[probe]` lines, including the automatic follow-ups 1 s and 3 s
  after a floor change ([light-probe.md](light-probe.md)).
- The `[furniture]` lines come from the lot light bridge; `[room]`, `[solve]` and the wall seams come from Level Light
  Share ([level-light-share.md](../night-lighting/level-light-share.md)).
- The recording and the two lighting captures need Night Lights on; the Report page says so when it is off.
- The post-save notes form does not open while a recording is running ([bug-reports.md](../bug-reports.md)).

## Limitations

- At most 20 seconds per recording.
- `[furniture]` and `[probe]` lines share a cap of 40,000; `[room]` lines have their own cap of 20,000. The file says
  which one stopped.
- `Wall seams.csv` keeps the latest 8,192 wall samples (a ring buffer); older samples of a long recording are dropped.
- Status lines are checked every 100 ms; a change that reverts within that interval is not seen.

## Technical reference

### Lifecycle

`Recorder::OnPresent` runs every frame. Order of handling: a cancel request wins over everything (shortcut, Stop and the
20-second deadline), then a stop request, then the toggle (shortcut or menu, consumed once), then the 100 ms status
check and the deadline (`kMaxMs = 20000`, `kStatusEveryMs = 100`).

- **Start**: `LevelLightShare::BeginSeamRecording()`; records the log file size (to read only new lines later) and a
  copy of `ApexRadiance.toml`; `LotLightBridge::FurnitureTraceReset()` so every object is written again at its first
  draw; writes every indoor room once (`LevelLightShare::TraceRooms(true)`); first status check. Log
  `[Recorder] Recording started (its shortcut again to stop; stops by itself after 20 s)`.
- **Stop**: `LevelLightShare::EndSeamRecording(true)`, last status check, log `[Recorder] Recording stopped`; reads the
  log lines written since the start (lines with an `hh:mm:ss.mmm` clock), adds `LevelLightShare::JournalSince(start)`,
  stable-sorts all lines by clock, creates `Captures\<date time> Recording\`, writes `Recording.txt` and
  `Wall seams.csv`, logs `[Recorder] Saved N lines to Captures\<folder>`, and calls `Captures::Finish` (log, settings,
  crash file, `About this capture.txt`, notice "Recording saved. Open Report a problem to find your files").
- **Cancel** (`RequestCancel`): ends the seam recording without saving, clears every buffer, logs
  `[Recorder] Cancelled: no capture folder created` and shows "Recording cancelled. No capture was saved".
  `RequestStop` and `RequestCancel` exist in the API and are exercised by the offline check; no menu control calls them.

In development builds with `release_debug_buffers` on, a recording's lines, views, notes, edits, the settings copy and
the furniture and room tracer states are given back when it is saved or cancelled, instead of at the next start.

### `Recording.txt`

Header `Apex Radiance recording: <start> to <end> (<s> s), <N> lines`, the legend, cap notices, then one line per event,
then `==== ApexRadiance.toml at the start of the recording ====` and the settings.

| Prefix | Source | Content |
|---|---|---|
| `[log]` | `ApexRadiance_LOG.txt` | Log lines written during the recording |
| `[solve]` | Level Light Share solve journal | `S` ambient step done, `W` wall pass done, `Q` sent by Apex, `H` held by Apex, `I` / `F` invalidated, with the caller, LOD class, light counts, lot and camera story |
| `[status]` | `Status()` every 100 ms, written when changed | Indoor light between stories (`LevelLightShare::Status`), Rooms at Night (`UnlitRooms::Status`), Faster Room Lighting (`RoomLightQueue::StatusText`), Indoor object maps (`RoomMapPadding::Status`), "Furniture (last 100 ms)" with the night level, "Stories shown (lot:story)" (a floor switch is a new line), "Rooms at Night sliders" (`UnlitRooms::SettingsText`), "Rooms keep their light" (`LampMarkFilter::Status`) |
| `[furniture]` | `LotLightBridge` | Each room-mode object part (world position from the VS world rows + pixel shader) at its first draw and at every change of: the path (A = Apex's indoor-object shader, B = the game's shader turned by Rooms at Night, game = untouched), dark (the rig holds [NoLight] lights) and acting, the 4 rig lights as the game set them (N = [NoLight], F = fill, L = lamp, - = empty) with their colours, the vertex lights' sum, the ambient cube weight (game -> drawn), the blue kept, and for path A the room light map, its first directional map and the read scale. Cheap hashes decide when a line is written |
| `[room]` | `LevelLightShare::TraceRooms` | Every indoor room of the loaded lots once at the start and again when its ambient (+0x110, the colour its walls take; +0x120), normalisation, solve state, LOD class (solving / shown), light count or shown story changes |
| `[probe]` | `LightProbe` | Each capture with its reason, and `[probe] floor change (...): automatic captures in 1 s and 3 s` |

### Light update trace

Three parts measure how the light updates on screen after a lamp edit, all inside the recording, nothing to arm:

- **Screen pixels** (`features/screen_watch.{h,cpp}`, started from Night Lighting's Present next to the Light Probe): 50
  points copied from the back buffer at every frame with `StretchRect` into a 50×1 render target (a ring of 4, each with an
  event query) and read back with `GetRenderTargetData` only once its query is done, so the frame never waits. Up to 4000
  samples. Released at the end and before a device Reset (`RenderCallbacks::preReset`).
  - A column of 9 at the mouse's pixel when the recording starts (±0.7, 1.85, 3.75, 7.5% of the height: ±15, 40, 81, 162 px
    at 2160). Point the mouse at the line between two floors: both stories are measured at once. The screen's centre when
    the mouse is outside the game or over the Apex menu (`Overlay::MouseOverMenu`: ImGui wants the mouse).
  - A grid of 6 × 4 over the screen (x 10, 26, 42, 58, 74, 90%; y 18, 38, 58, 78%, above the game's bottom bar), so a
    recording started from the menu still measures the house.
  - 17 ground points (`GroundProfile::WatchPoints`, [`features/ground_profile.cpp`](../../../features/ground_profile.cpp)):
    at the first frame with a camera and listed lamps, the active lot's lamp nearest the mouse's ground point is chosen,
    and the points lie every metre from 4 m behind it to 12 m past it on the line towards that point, at its lot's
    ground height, following the lamp as it moves. Their pixels come from the post-scene camera vote, asked for while the
    recording runs (`PostScene::WantCamera`); a point off screen reads pixel (0, 0).
  - Copied at `RenderCallbacks::filteredSceneBeforeOverlay` (the first EndScene of the frame, right before the Apex menu
    and notices are drawn, as the report screenshot does), at Present only when no EndScene reached the overlay that frame,
    so a recording started from the open menu measures the game and not the menu's own pixels.
  - `Screen pixels.csv`: elapsed ms, then luma, r, g, b per point; the column's names carry the row offset (`luma+0` = the
    mouse or the centre), the grid's their place in percent (`luma_x26y38`), the ground points' their distance from the
    lamp (`luma_ground+3` = 3 m past it).
  - `Ground timeline.csv` (when a lamp was chosen; at most 4000 rows): per frame, the elapsed ms, the lamp's place, the
    versions of the chunk under it (game map, smoothed build, re-renders, stamp version and build, stamps now), whether
    the bake excludes the lamp, its [terrain stamp](../night-lighting/terrain-relight.md#dragged-lamps-on-the-gpu) state
    and each ground point's pixel (-1 off screen).
- **Room solve ends**: `FUN_006a0e00` (step 8 of the budgeted solve, its call at `0x6A3E65` in `FUN_006a3c90`, Steam,
  checked at install) unlocks the maps the solve wrote and gives them to the room, which shows them from the next frame.
  It is noted `E` in the solve journal, and during a recording the journal keeps every room (not only those sharing light
  through an opening). The same hook tells Faster Room Lighting when a quick pass is shown
  ([room-light-queue.md](../performance/room-light-queue.md)).
- **Lamp edits**: `LampMarkFilter` notes every player edit (a switch, a move or a value change; not a light switching
  itself) as `[edit]` lines, one per light and story tree level that saw it first (a lamp object has several lights; the
  game marks the rooms of every story its light reaches, so the story is where the change was seen, not always the lamp's).

`Recording.txt` then opens with **Light updates**, one block per edit burst (edits less than 400 ms apart). A burst's window
ends at the next burst, or when its lot shows another story or leaves the view (the "Stories shown" status, every 100 ms,
so a trip to the map view does not count as more time of the switch). Per burst:

- the lamp lights (on, off, moved) and the stories that saw them, and the story the lot showed;
- the rooms of that lot that showed new light: a room counts from its first invalidate or send in the window, and its
  first solve end after it is when its new light appears (rooms that only finished a solve started before are counted
  apart); when all of them had it, how many took the quick pass first, the last solve;
- per story: its rooms, when its new light appeared (first .. all rooms), its last solve; between stories, how far apart
  each story had all its new light and its last solve;
- the atrium rooms (new light and last solve) and their spread;
- the rooms solved more than once, with each solve's time and the class it then showed (0 = the quick pass, 2 = its
  refinement; the `E` note is taken after the finalize, so "shown" +0x100 is that solve's class);
- the column's points: value before and after, when it began to change, when it settled, its reversals and
  `RIGHT THEN WRONG` when it reached its final value and then left it again by more than 6 luma levels, and how far apart
  the points above and below the mouse settled;
- the grid: how many points changed, when they settled, how many reversed, the `RIGHT THEN WRONG` ones with their place,
  and every changed point;
- the ground: the terrain chunks whose smoothed light map showed new light (`Recorder::NoteGround` from LightmapSmooth's
  builds, not border-only rebuilds), when, and whether in one frame (a lamp switch's held ground) or one by one. Each is
  also a `[ground]` line in the timeline.

### `Wall seams.csv`

Written by `LevelLightShare::EndSeamRecording`. Columns:

```
elapsed_ms,lot,story,room,class,x,y,z,nx,ny,nz,story_base,raw_r,raw_g,raw_b,normalization,after_curve_sum
```

| Column | Meaning |
|---|---|
| `elapsed_ms` | Time since the recording started |
| `lot` | Low 32 bits of the lot id (tracker +0x90), hex |
| `story`, `room`, `class` | Manager story (mgr+0x88), room id (room+0xC), LOD class (room+0xF4) |
| `x,y,z`, `nx,ny,nz` | Sample position and normal |
| `story_base` | The story's lowest floor height (mgr+0x98) |
| `raw_r,raw_g,raw_b` | The solve's output colour |
| `normalization` | room+0x160 |
| `after_curve_sum` | Normalised colour sum after the game's curve (`2/(1+e^-lum) - 1) / lum`, `FUN_0069ec40`) |

Samples come from `RecordRequestedSeam`, called after each batched solve point (not ghost solves) while recording:
walls only (normal |ny| <= 0.3), indoor rooms only (room+0x18 = 0), and only within 0.025 m of the story base or of
base + 3 m (the floor lines with the story below and above). Ring buffer `kRecordedSeamLimit = 8192`; an epoch counter
discards samples from a previous recording.

### Files and functions

| File | Symbol | Role |
|---|---|---|
| `features/recorder.h/.cpp` | `Recorder::Active`, `Verbose`, `Note`, `NoteLampEdit`, `SecondsRecorded`, `RequestToggle`, `RequestStop`, `RequestCancel`, `JustSaved`, `OnPresent` | API |
| | `Start`, `Stop`, `Status`, `FurnitureLine`, `CameraLine` (keeps `g_views`), `Clock` | Lifecycle and lines |
| | `LightUpdates`, `TellPoint`, `PixelCsv`, `StoryShown` | Light updates summary, `Screen pixels.csv` |
| `features/screen_watch.h/.cpp` | `ScreenWatch::OnPresent`, `Samples`, `PointX`, `PointY`, `Width`, `Height`, `ColumnAtCentre` | Screen pixels |
| `features/ground_profile.cpp` | `GroundProfile::WatchBegin`, `WatchPoints`, `WatchEnd`, `WatchCsv` | Ground points, `Ground timeline.csv` |
| `features/level_light_share.cpp` | `BeginSeamRecording`, `EndSeamRecording`, `RecordRequestedSeam`, `MakeSeamRec`, `TraceRooms`, `JournalSince` | Seams, rooms, solve journal |
| `features/lot_light_bridge.cpp` | `FurnitureTraceReset`, `FurnitureDiag`, furniture trace | `[furniture]` lines |
| `apex_gui.cpp` | Report page row, top notice | UI |

## Rejected approaches

- One shared line cap for furniture, probe and room lines
  ([history](../../history/dev-tools-recorder.md#2026-09-30-separate-caps)).

## See also

- [Validation](../../validation/dev-tools-recorder.md)
- [History](../../history/dev-tools-recorder.md)
- [Report a problem](../bug-reports.md), [Light Probe](light-probe.md), [Level Light Share](../night-lighting/level-light-share.md)

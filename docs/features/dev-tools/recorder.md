# Recorder (lighting recording)

The recorder captures up to 20 seconds of lighting activity while the player reproduces a problem. Every line carries
its clock time and the lines are sorted together: the log, the room light solves, status changes of the lighting
modules, every indoor object (furniture) whose drawing changes, every indoor room whose light changes, and Light Probe
captures. Players start it as **Record a few seconds** on the Report a problem page or with its shortcut; each
recording goes to its own capture folder, with a `Wall seams.csv` of the wall light at the floor lines.

## Status

| | |
|---|---|
| Availability | Released in 2.5.0 as a player capture (Report a problem). `Wall seams.csv`: In development (PR #2) |
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
| `features/recorder.h/.cpp` | `Recorder::Active`, `Verbose`, `Note`, `SecondsRecorded`, `RequestToggle`, `RequestStop`, `RequestCancel`, `JustSaved`, `OnPresent` | API |
| | `Start`, `Stop`, `Status`, `FurnitureLine`, `CameraLine`, `Clock` | Lifecycle and lines |
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

# Recorder: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/dev-tools/recorder.md](../features/dev-tools/recorder.md).

### 2026-09-30: the recorder and the furniture tracer

**Context:** indoor furniture changed colour after floor switches, and single captures could not show the sequence. The
first recorder was in the development build; the shortcut (Ctrl+Shift+X / 6 / F6 by preset) wrote
`Documents\...\Apex Radiance\ApexRadiance_Recording_<hhmmss>.txt`.

**Finding:** the `[furniture]` and `[room]` lines were added the same day. Rooms were written only once they changed, so
a recording did not show a room's state before the problem.

**Outcome:** every room is written once at the start. The Light Probe gained automatic captures 1 s and 3 s after a
floor change. Recommended procedure for a furniture problem with a floor switch: point at the object and take a light
capture (the "before"); start the recording, switch floors (and back), and let it stop by itself; the automatic
captures and the object's `[furniture]` lines (found by its world position, for example from the capture's VS world
rows) and every room solve are then in the files.

### 2026-09-30: separate caps

**Context:** recording F6 105204.

**Finding:** a Brightness drag filled the shared 40,000-line cap with `[furniture]` lines in 4 s, and the room tracer
stopped before the room that went wrong.

**Outcome:** `[room]` lines got their own cap of 20,000; `[furniture]` and `[probe]` keep 40,000. The file says which
one stopped.

### 2026-09-30: player capture

**Context:** the Report a problem page was built for players.

**Finding:** a single file per recording in the Apex Radiance folder was hard for players to find and send.

**Outcome:** the recorder runs in both builds, writes `Captures\<date time> Recording\Recording.txt` and is never
overwritten ([bug-reports.md](../features/bug-reports.md)).

### 2026-10-05: wall seams file

**Context:** PR #2 work on the seam between stories in atrium walls.

**Finding:** the snapshot's seam section needed the diagnostics armed and showed only the latest solves.

**Outcome:** each recording also writes `Wall seams.csv`, with the wall samples at the floor lines from every batched
solve during the recording, in normal mode too.

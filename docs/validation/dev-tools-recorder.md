# Recorder: validation

The feature is described in [features/dev-tools/recorder.md](../features/dev-tools/recorder.md). It must:

- Start and stop exactly once per request, whether it comes from the menu, the shortcut or both in the same frame.
- Stop by itself after 20 seconds and save one capture folder.
- Save nothing when cancelled, even when the deadline is due in the same frame.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/report_check/recorder_check.cpp`](../../tools/report_check/recorder_check.cpp) | The production request, cancel and deadline bodies, extracted by `extract_menu.py` | See the [harness README](../../tools/report_check/README.md) | x86 MSVC |
| [`tools/report_check/menu_253_check.cpp`](../../tools/report_check/menu_253_check.cpp) | The Report page Start / Stop row and the recording notice (recorder stubbed) | Same | x86 MSVC, ImGui from vcpkg |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| Not recorded | PR #2 | `recorder_check` | Prints "10 recording request/cancel/auto-save scenarios passed using production bodies" when it passes | None |

## In-game test plan

1. Night Lights on, indoors. Press the Recorder shortcut (`X`, `6` or `F6`). **Expected:** the notice "Recording N s ·
   press <key> to stop" with a red pulsing icon.
2. Switch floors and back, then wait. **Expected:** the recording stops by itself at 20 s; notice "Recording saved.
   Open Report a problem to find your files".
3. Open the folder. **Expected:** `Recording.txt` with `[solve]`, `[status]`, `[room]` and `[furniture]` lines in time
   order and the settings at the end; `Wall seams.csv` with the header row and wall samples near the floor lines.
4. Start from Report a problem > Record a few seconds > Start, stop with Stop. **Expected:** the same files; other
   capture buttons are disabled while recording.
5. Take a light capture during a recording. **Expected:** a `[probe]` line.

## Confirmed in game

- Recording F6 105204 exposed the shared-cap problem fixed with separate caps ([history](../history/dev-tools-recorder.md)).

## Open checks

- `Wall seams.csv` in game: confirm samples appear on an atrium house and that their values match the seam seen on
  screen.
- No menu control calls `RequestStop` or `RequestCancel`; cancellation is exercised only by the offline check.

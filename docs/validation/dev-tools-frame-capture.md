# Frame Capture: validation

The feature is described in [features/dev-tools/frame-capture.md](../features/dev-tools/frame-capture.md). It must:

- Do no work while idle, and record exactly two frames after the next Present once armed.
- Never install in normal mode.
- Write a readable file even when the frame exceeds the line cap, and cancel cleanly when the game never presents.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| None | | | |

The recording needs a D3D9 device and the normal-mode refusal needs the patch manager; neither has an offline harness.
[`tools/developer_mode_check`](../../tools/developer_mode_check/check.cpp) covers the developer-mode default and
profile filtering only ([validation/developer-mode.md](developer-mode.md)).

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | No dedicated frame capture result is recorded | | |

## In-game test plan

1. Developer mode on, restart. Developer > Captures > Capture two drawn frames: turn the switch on. **Expected:** the
   log shows `[FrameCapture] Installed ...`.
2. Press Capture two frames or the shortcut (`F`, `7` or `F5`). **Expected:** status goes through
   `Waiting for the next frame...` and `Capturing...` to `Capture saved (N lines) to ApexRadiance_FrameCapture.txt`.
3. Open the file in the Apex Radiance folder. **Expected:** header with depth-format support, `==== FRAME 1 ====` and
   `==== FRAME 2 ====`, the `---- Game's EndScene` marker separating game draws from overlay draws.
4. Search `StretchRect BACKBUFFER` and `type=5 ... prims=2 ... z=0/0`. **Expected:** the scene copy and the full-screen
   passes are found.
5. Developer mode off, restart. **Expected:** no Frame Capture card or shortcut row; a saved `enabled = true` is ignored.

## Confirmed in game

- The captures recorded in [history](../history/dev-tools-frame-capture.md) established the scene/UI boundary and
  readable depth support used by Depth Blur and the post-scene chain.

## Open checks

- The patch description and the install log line still name Ctrl+Shift+F9; the shortcut is the `FrameCapture` preset
  key (`F5` in the F-key preset). Code text to update.

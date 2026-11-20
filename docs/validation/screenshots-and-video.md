# Screenshots and video: validation

The feature is described in [features/screenshots-and-video.md](../features/screenshots-and-video.md). It must:

- Save screenshots with Apex's effects and the ReShade look, in the format, size and folder ApexCapture.ini says.
- Keep the menu, ApexCapture.ini and the `[ui]` screenshot keys in step both ways.
- Record MP4 video with the look and Apex colour, without the Apex menu or notes unless asked for.
- Stop cleanly at the length and size limits, on a device reset, and when the game closes, leaving a playable file.
- Refuse to start, with a note, when address space is short, with multisampling, or with an unknown back-buffer format.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| Smoke suite | Build and the shared offline checks | `tools\development\check.ps1 -Suite Smoke` | MSVC x86 |
| Capture format check | A 10-bit render target and back buffer copied to 8-bit by `StretchRect` (NONE and LINEAR), colour read back | `tools/capture_format_check` (README) | MSVC x86, a D3D9 GPU |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-11-20 | 0a0a4e25 (release/4.0.13) | In-game runner, ApexTest-Night at 3840x2160: 20 s video at 4K 60 fps HEVC, then 1080p 30 fps H.264 | 4K: 1097 frames over 18.5 s (59.4 fps), 0 dropped, plain MP4; 1080p: 551 frames over 18.4 s (30.0 fps), 0 dropped, even 33.3 ms spacing | RTX Remix bridge (#126 build) |
| 2026-11-20 | release/4.0.13 | Offline Media Foundation probe (NVIDIA encoders), NV12 and RGB32 input, fragmented and plain MP4 sinks | HEVC refused at `BeginWriting` (`0xC00D36B4`) by the fragmented sink at 1080p and 4K; plain MP4 and H.264 in both sinks work | None |
| 2026-11-20 | 4.0.12 recorder on 4.0.13's code before the pacing fix | 1080p 30 fps video | 746 frames in 18.6 s (about 40 fps), repeated timestamps | RTX Remix bridge |
| 2026-11-19 | release/4.0.9 | Capture format check | Passed: 336699 read back from both 10-bit sources with both filters | Native D3D9, NVIDIA |
| 2026-11-14 | PR branch | Smoke suite | Passed | None |
| 2026-11-14 | 6756d30f | In-game runner `capture` (F8, a bare F12 as the record key) | 3840x2160 PNG (14.0 MB) and a 12 s MP4 (22.6 MB) written to the run folder | RTX Remix bridge (tester's game) |

## In-game test plan

The in-game runner's `capture` job covers steps 1 and 4 without a person: F8 and a 12-second video (record key set to
F12 for the run) into the load's own folder, a non-empty PNG and MP4 and `[Video] Video saved` in the log. Run it with
the 4.0.0 queue ([tools/ingame_runner/README.md](../../tools/ingame_runner/README.md#shaders-and-capture-jobs)); whether
the picture and video look right, and the sound, stay manual checks.

1. F8 in a world with a look on. **Expected:** a PNG with the look in the screenshot folder, named as FileNaming says.
2. *Look* set to Both, F8. **Expected:** a second picture ending in `_before` without the look.
3. Burst of 5 at 500 ms. **Expected:** five pictures, about half a second apart.
4. Shift+F8, record 30 s, Shift+F8. **Expected:** an MP4 that plays with sound; the timer pill shows while recording and
   is not in the video.
5. MaxLength 60 with WarnBefore 20. **Expected:** the timer turns amber with the time left and the recording stops at 60 s.
6. Edit ApexCapture.ini while the game runs. **Expected:** the menu shows the new value about a second later.
7. Switch capture profile by key. **Expected:** the next profile's settings are used for the next shot.

## Confirmed in game

- Nothing yet.

## Open checks

- Every step of the in-game test plan.
- HEVC on hardware without an HEVC encoder (falls back to H.264).
- A screenshot and a video with a 10-bit back buffer in game (a separate `BandingFix.asi`), under the RTX Remix bridge
  and on the game's own Direct3D 9.

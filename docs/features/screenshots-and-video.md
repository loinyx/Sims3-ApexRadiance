# Screenshots and video

Code: `features/apex_capture.cpp/.h` (settings file, menu rows, video control), `features/video_mp4.cpp/.h` (MP4 writer),
`features/captures.cpp` (`RequestPlayerScreenshot`, the screenshot jobs). Menu: Settings > Menu, cards **Screenshot
capture** and **Video recording**; the keys are on Settings > Shortcuts (Take a filtered screenshot, Record a video). Validation: [validation/screenshots-and-video.md](../validation/screenshots-and-video.md).

## The settings file

`Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexCapture.ini`: plain text, a note above every key, sections
[GENERAL], [SCREENSHOT], [VIDEO] (screenshot keys named as in GShade.ini). Written with the defaults when missing (the
first time seeded from `[ui] screenshot_key`, `screenshot_hide_game_ui`, `screenshot_folder`); re-read about a second
after it is saved while `ReloadOnChange=1`; written half a second after a menu change. A value Apex does not understand
keeps its default and is named in a note and the log; unknown keys are kept. Deleting the file writes the current values
again. Settings > Menu > Reset all settings also resets it (not part of the Undo).

`[ui] screenshot_key`, `screenshot_hide_game_ui`, `screenshot_folder` / `screenshot_custom_folder` and the file's
`KeyScreenshot`, `IncludeGameUI`, `SavePath` are kept the same both ways (a change on either side wins).

| Section | Key | Default | Values |
|---|---|---|---|
| GENERAL | ReloadOnChange | 1 | 1 / 0 |
| GENERAL | ShowNotice | 1 | 1 / 0 |
| GENERAL | SoundPath | (empty) | a .wav path |
| SCREENSHOT | KeyScreenshot | F8 | Apex key text or GShade numbers (`107,0,1,0`) |
| SCREENSHOT | SavePath | %GameScreenshots% | folder; tokens %GameScreenshots% %ApexFolder% %Pictures% %Videos% %Documents%, environment variables |
| SCREENSHOT | FileNaming | %Date%_%TimeHour%-%TimeMinute%-%TimeSecond%-%TimeMS%_%PresetName% | + %Year% %Month% %Day% %Counter% %Look% |
| SCREENSHOT | CounterDigits | 4 | 1-8 (counter kept in `ApexCapture_counter.txt`) |
| SCREENSHOT | FileFormat | png | png / jpg (GShade's 0-2 read too) |
| SCREENSHOT | JPEGQuality | 95 | 1-100 |
| SCREENSHOT | ResolutionScale | 100 | 10-100 % |
| SCREENSHOT | IncludeGameUI | 0 | 1 / 0 (0: F10 for one frame) |
| SCREENSHOT | IncludeApexMenu | 0 | 1 / 0 |
| SCREENSHOT | SaveBeforeShot | 0 | 1 / 0 (`<name>_before`, same frame without the look) |
| SCREENSHOT | SavePresetFile | 0 | 1 / 0 (the preset .ini next to the picture) |
| SCREENSHOT | BatchQuantity | 1 | 1-50 |
| SCREENSHOT | BatchInterval | 500 | 50-10000 ms |
| VIDEO | Enabled | 1 | 1 / 0 |
| VIDEO | Engine | builtin | builtin / external |
| VIDEO | KeyRecord | Shift+F8 | key text |
| VIDEO | SavePath | %GameScreenshots%\Videos | as screenshots |
| VIDEO | FileNaming | %Date%_%TimeHour%-%TimeMinute%-%TimeSecond%_%PresetName% | as screenshots |
| VIDEO | Container | mp4 | mp4 / mp4-safe (fragmented; `mkv` reads as mp4-safe) |
| VIDEO | Codec | h264 | h264 / hevc (falls back to h264) |
| VIDEO | FPS | 30 | 10-60 (menu: 24 / 30 / 60) |
| VIDEO | RateControl | quality | quality / bitrate |
| VIDEO | Quality | 80 | 1-100 (720p 8, 1080p 16, 1440p 28 Mbit/s at 30 fps and 80; x1.6 at 60 fps) |
| VIDEO | BitrateMbps | 20 | 1-200 |
| VIDEO | MaxHeight | 1080 | 360-2160 lines (menu: 720 / 1080 / 1440) |
| VIDEO | Audio | game | game / off (process loopback, Windows 10 2004+) |
| VIDEO | IncludeGameUI | 1 | 1 / 0 (0: F10 while recording) |
| VIDEO | IncludeApexMenu | 0 | 1 / 0 |
| VIDEO | MaxLength | 600 | 0-36000 s (0 = no limit) |
| VIDEO | MaxFileMB | 4000 | 0-100000 MB (0 = no limit) |
| VIDEO | ShowRecordingDot | 1 | 1 / 0 (the recording timer: red dot, time, file size, stop key) |
| VIDEO | TimerPosition | top-center | top-left / top-center / top-right / bottom-left / bottom-right |
| VIDEO | TimerSize | 100 | 70-160 % |
| VIDEO | TimerOpacity | 90 | 30-100 % |
| VIDEO | TimerBlink | 1 | 1 / 0 (the dot pulses) |
| VIDEO | TimerHours | 0 | 1 / 0 (0:02:15) |
| VIDEO | TimerStopKey | 1 | 1 / 0 |
| VIDEO | TimerFileSize | 1 | 1 / 0 (built-in recorder) |
| VIDEO | TimerInVideo | 0 | 1 / 0 (1: the video is read at Present, so an open menu and notes are in it too) |
| VIDEO | RedFrame | 0 | 1 / 0 (thin red border while recording) |
| VIDEO | WarnBefore | 60 | 0-600 s before MaxLength / MaxFileMB (estimated from the rate so far): amber + time left |
| VIDEO | SavedNote | 1 | 1 / 0 ("Video saved: 04:32 · 545 MB"; the full line goes to the log) |
| VIDEO | ExternalKey | Win+Alt+R | key text (Engine=external) |

## How the pictures get the look

Screenshots are read at Present (menu closed or hidden for the shot), after the GShade look and Apex colour, or at
Present with the menu when IncludeApexMenu=1. Video frames are grabbed at `filteredSceneBeforeOverlay`; while recording,
the bootstrap runs Picture before the overlay (`ApexCapture::NeedsGradeBeforeOverlay`), so the frame has the look and the
colour but not the menu or the notes (IncludeApexMenu=1 grabs at Present instead). The "before" copy is taken in
`gshade_looks.cpp` right before the look runs (once per frame, at the first part when the look is split in a world),
only while a screenshot asks for it. With "Keep UI text sharp" on in a world that first part runs before the HUD, so
the "before" picture has no HUD; this only shows when IncludeGameUI=1.

Video: the frame is scaled on the GPU to MaxHeight, read back through a ring of 3 slots without waiting (a frame with no
free slot is dropped and counted), and encoded by Media Foundation (hardware encoder when present). Start refuses when
the largest free address-space block is under 192 MB, with multisampling, or with an unknown back-buffer format. A
device reset stops the recording.

## The recording timer

`ApexCapture::DrawTimer` (called from the menu's `Draw` every frame while recording; `AlwaysDraw` stays on) draws a pill in
the Apex note style on the foreground list: dot, time, size, stop key, or in amber the time left within WarnBefore. Top
centre moves under an Apex note when one shows. It is skipped while a screenshot is taken, and hidden when the menu is
recorded but TimerInVideo=0 (it would end up in the video). The external recorder gets the time only (from when Apex sent
its key).

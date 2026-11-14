# Screenshots and video

Code: `features/apex_capture.cpp/.h` (settings, the Capture page's cards, video control), `features/video_mp4.cpp/.h`
(MP4 writer), `features/captures.cpp` (`RequestPlayerScreenshot`, the screenshot jobs). Menu: Image > Capture, tabs
**Screenshots** and **Video**; the keys are on Settings > Shortcuts (Take a filtered screenshot, Record a video, The other
app's key). Validation: [validation/screenshots-and-video.md](../validation/screenshots-and-video.md).

## Menu

Page "Capture" (Lucide `camera`, after Display), subtitle "Screenshots and videos with your effects". Each tab is one
card. Its header (`ApexUi::CardStatusHeader`) has one status line and one action; the action moves under the text when
the card is narrow.

| Tab | Status line | Action |
|---|---|---|
| Screenshots | the screenshot key, then "Last: <file>" and a **Show** link (Explorer with the file selected) once a picture was saved this session | **Take one** (primary): the same request as the key (`RequestPlayerScreenshot` with Hide game UI) at the next frame; the menu is hidden for the picture unless Include the Apex menu is on |
| Video, idle | the record key · "about N MB a minute" (`KbpsFor`), then "Last: <file>" and **Show** once a video was saved this session | **Record** (primary) |
| Video, recording | "Recording m:ss · N MB" in the error colour after a red dot ("Recording m:ss" with the other app) | **Stop** |
| Video, saving | "Saving the video…" | Record, greyed out |

Record is greyed out, with one warning note under the card, while a video cannot start (`ApexCapture::CannotRecord`):
the game's own Edge Smoothing (multisampling) is on, or the game has no free 192 MB block of addresses (checked at most
once a second), or Media Foundation is missing (Windows N without the Media Feature Pack; its DLLs are delay-loaded, so
Apex still loads without them).

The card body, in this order. Both tabs use the same group names; the rows are the menu's row widgets, so search, the
changed dot, per-row Reset and Undo work.

| Group | Screenshots | Video |
|---|---|---|
| Where | Save to (Game / Apex / Other; the folder in use as its description; an Open the folder icon button beside the choices); with Other, Folder (Change: Windows' folder dialog; the typed path, with tokens, under a *Type a folder* arrow) | the same |
| In the picture / In the video | Hide game UI; Look (With / Without / Both, only while a ReShade look is on) | Hide game UI, Game sound, Look (With / Without, only while a look is on) |
| Quality | File type (PNG / JPG) | Quality (Smaller / Balanced / Best / Custom; description "<size>p · <fps> fps · about N MB a minute") |
| Advanced: File / Picture | File: JPG quality (JPG only), Size (%), File name (token help on hover) | Picture: Size (720p / 1080p / 1440p), Frames per second (24 / 30 / 60), Custom bitrate (Custom only); File: File type (MP4 / Crash-safe MP4 / MKV), Keep the MP4 too (MKV only), Compression (H.264 / HEVC), File name |
| Advanced: Also in the picture / video | Include the Apex menu, Include the mouse pointer, Save the look's preset too | the same |
| Advanced: Series / Length | Pictures per press, Time between pictures (more than one picture) | New clip every, Stop after, Stop at file size, Warn before the limit |
| Advanced: Timer on screen | | Show a recording timer (then Timer position, Timer size, Timer opacity, Pulse the red dot, Show the file size, Show the stop key, Always show hours), Red frame while recording, Timer in the video too |
| Advanced: Recorder | | Recorder (Apex / Another app; with Another app a note says its key is on the Shortcuts tab) |
| Advanced: When saved | Sound (Off / Camera / Own file, with Sound file), Note when saved | the same (`[capture]`, shared) |

## Settings

`ApexRadiance.toml`, saved with the other settings. The screenshot key, folder and Hide game UI are `[ui]`
(`screenshot_key` F8, `screenshot_folder` game / apex / custom, `screenshot_custom_folder`, `screenshot_hide_game_ui`
true; see [ui.md](../ui.md#shortcuts)). Every value is clamped when read; missing keys take the defaults.

| Table | Key | Default | Values |
|---|---|---|---|
| capture | sound | off | off / camera / a .wav path |
| capture | notes | true | a short note when a picture or video is saved |
| capture | counter | 1 | 1-99999999, the next %Counter% |
| capture.screenshot | file_name | Screenshot_%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%_%TimeMS% | tokens (hover the box) |
| capture.screenshot | include_menu | false | |
| capture.screenshot | include_pointer | false | |
| capture.screenshot | look | with | with / without / both (`<name>_before`, the same frame without the look) |
| capture.screenshot | save_preset | false | the preset .ini next to the picture |
| capture.screenshot | file_type | png | png / jpg |
| capture.screenshot | jpg_quality | 95 | 1-100 |
| capture.screenshot | size_percent | 100 | 10-100 % |
| capture.screenshot | burst_count | 1 | 1-20 |
| capture.screenshot | burst_interval_ms | 500 | 50-10000 ms, the whole series within 30 s |
| capture.video | folder | game | game (Recorded Videos) / apex (Apex Radiance, Videos) / custom |
| capture.video | custom_folder | (empty) | folder; tokens and environment variables |
| capture.video | file_name | Video_%Date%_%TimeHour%-%TimeMinute%-%TimeSecond% | tokens |
| capture.video | hide_game_ui | false | F10 while recording |
| capture.video | include_menu | false | the video is read at Present |
| capture.video | include_pointer | false | |
| capture.video | look | with | with / without |
| capture.video | save_preset | false | |
| capture.video | size | 1080 | 720 / 1080 / 1440 lines at most |
| capture.video | fps | 30 | 24 / 30 / 60 |
| capture.video | quality | balanced | smaller / balanced / best / custom (balanced: 720p 8, 1080p 16, 1440p 28 Mbit/s at 30 fps; x1.6 at 60 fps) |
| capture.video | custom_mbps | 20 | 1-200 (menu 1-100) |
| capture.video | container | mp4-safe | mp4 / mp4-safe (fragmented) / mkv (made from the MP4 when the video stops) |
| capture.video | keep_mp4 | false | with mkv |
| capture.video | codec | h264 | h264 / hevc |
| capture.video | game_sound | true | process loopback, Windows 10 2004+ |
| capture.video | new_clip_mb | 0 | 0-100000 MB (0 = one file) |
| capture.video | stop_after_s | 600 | 0-36000 s (0 = no limit) |
| capture.video | stop_at_mb | 4000 | 0-100000 MB (0 = no limit) |
| capture.video | warn_before_s | 60 | 0-600 s: amber and the time left |
| capture.video | timer | true | red dot, time, file size, stop key |
| capture.video | timer_position | top-center | top-left / top-center / top-right / bottom-left / bottom-right |
| capture.video | timer_size | 100 | 70-160 % |
| capture.video | timer_opacity | 90 | 30-100 % |
| capture.video | timer_blink | true | |
| capture.video | timer_file_size | true | built-in recorder |
| capture.video | timer_stop_key | true | |
| capture.video | timer_hours | false | 0:02:15 |
| capture.video | red_frame | false | |
| capture.video | timer_in_video | false | the video is read at Present, so an open menu is in it too |
| capture.video | recorder | apex | apex / external |
| capture.video | external_key | Win+Alt+R | a chord with a modifier (recorder = external) |
| capture.video | record_key | Shift+F8 | key text |

## How the pictures get the look

Screenshots are read at Present (menu closed or hidden for the shot), after the ReShade look and Apex colour, or at
Present with the menu when include_menu is on. Video frames are grabbed at `filteredSceneBeforeOverlay`; while recording,
the bootstrap runs Picture before the overlay (`ApexCapture::NeedsGradeBeforeOverlay`), so the frame has the look and the
colour but not the menu or the notes (include_menu grabs at Present instead). The "before" copy is taken in
`patches/looks_patch.cpp` right before the look runs (once per frame, at the first part when the look is split in a world),
only while a screenshot asks for it. With "Keep UI text sharp" on in a world that first part runs before the HUD, so
the "before" picture has no HUD; this only shows when Hide game UI is off.

Video: the frame is scaled on the GPU to `size` lines, read back through a ring of 3 slots without waiting (a frame with no
free slot is dropped and counted), and encoded by Media Foundation (hardware encoder when present). Start refuses when
the largest free address-space block is under 192 MB, with multisampling, or with an unknown back-buffer format. A
device reset stops the recording.

## The recording timer

`ApexCapture::DrawTimer` (called from the menu's `Draw` every frame while recording; `AlwaysDraw` stays on) draws a pill in
the Apex note style on the foreground list: dot, time, size, stop key, or in amber the time left within warn_before_s. Top
centre moves under an Apex note when one shows. It is skipped while a screenshot is taken, and hidden when the menu is
recorded but timer_in_video is off (it would end up in the video). The external recorder gets the time only (from when Apex sent
its key). The pill's text is built again only when it changes (about once a second), and the video settings the frame
reads are copied only after the menu changed them, so a recording frame allocates nothing on the render thread for them.

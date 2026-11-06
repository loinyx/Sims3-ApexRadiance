# Offline Report checks

These fixtures access temporary capture files and an ImGui context. They do not run the game or validate its GPU, lighting, loading transitions or point selection callback.

`report_check.cpp` exercises capture storage, atomic required descriptions, collections, removal/Undo and real WIC image failure/retry. It also checks that the filtered player screenshot targets an isolated game's Documents `Screenshots` folder. `recorder_check.cpp` exercises the production request/cancel/wait/deadline implementation. `capture_ring_check.cpp` runs the production `screen_watch.cpp` and `capture_video.cpp` (compiled beside it) on a native D3D9 device: sample order, frames without ground points, video frames, dropped frames and a cancelled video, and the reading of a classes frame (`frame_classes.h`); pass a scratch folder. `capture_pack_check.py` runs `tools/capture_pack.py` on a made-up recording. `overlay_check.cpp` checks the clock and extracted loading gate against simulated state.

`menu_253_check.cpp` draws the current extracted Report page with the real Violet widgets, Segoe fonts, Lucide icons and translation tables. It renders the page across EN/PT/ES/FR and normal/narrow layouts, checks visible controls, and clicks the real buttons. It also checks notice centering and one-line text at three viewport widths and two font sizes. Recording and probe requests are stubbed; note files are real.

From the repository root, using an x86 Visual Studio developer prompt, Python 3 and the existing static x86 vcpkg dependencies, substitute scratch/vcpkg paths and quote paths with spaces:

```bat
mkdir <scratch>
python tools/report_check/extract_menu.py <scratch>/report_ui_under_test.inc
cl /nologo /std:c++20 /utf-8 /EHsc /MT /I. /Iui /Iframework /Ifeatures /I<scratch> /I<vcpkg>/include tools/report_check/menu_253_check.cpp <scratch>/widgets_recorded.cpp ui/violet_theme.cpp ui/icons.cpp ui/i18n.cpp framework/apex_util.cpp i18n/tr_features.cpp i18n/tr_image.cpp i18n/tr_lighting.cpp i18n/tr_menu.cpp i18n/tr_widgets.cpp /Fo<scratch>/ /Fe<scratch>/menu_253_check.exe /link /LIBPATH:<vcpkg>/lib imgui.lib user32.lib
<scratch>/menu_253_check.exe <scratch>
```

The extractor generates the Report implementation, recorder/gate slices and a temporary copy of `widgets.cpp` with only the `IconTextButton` entry point renamed. The fixture wraps that real entry point to collect button rectangles and disabled state for mouse clicks; it does not replace the drawing or translation logic. Generated files belong outside the source tree. Native PNG previews use CPU rasterization and real WIC output; preview success does not prove DX9 driver performance.

The notice portion extracts the production layout and checks 24 cases: four languages, three viewport widths and two font sizes. It verifies first-frame centering and full one-line text without ellipsis; long notices may exceed narrow viewports. These checks do not validate gameplay or GPU performance.
## Restored 2.5.3 RC page

The previous `menu_check.cpp` fixture tests the superseded guided redesign and remains historical. For the restored session/capture/list/help page, use `menu_253_check.cpp` with the Report block extracted through the Developer section. Include all `i18n/tr_*.cpp` tables. Current cases cover four languages, two widths/font scales, idle/recording/pending save/failure and the post-save notes form with required title and optional description. Capture storage is real and isolated; game APIs are inert. These are native UI/storage checks, not gameplay validation.


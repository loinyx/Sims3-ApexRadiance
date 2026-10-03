# Offline Report checks

These fixtures access temporary capture files and an ImGui context. They do not run the game or validate its GPU, lighting, loading transitions or point selection callback.

`report_check.cpp` exercises capture storage, atomic required descriptions, collections, removal/Undo and real WIC image failure/retry. It also checks that the filtered player screenshot targets an isolated game's Documents `Screenshots` folder. `recorder_check.cpp` exercises the production request/cancel/deadline implementation. `overlay_check.cpp` checks the clock and extracted loading gate against simulated state.

`menu_check.cpp` draws the extracted native Report page with the real Violet widgets, Segoe fonts, Lucide icons and translation tables. It renders 312 fixture frames across EN/PT/ES/FR and normal/narrow layouts, checks visible controls, and clicks the real buttons in 18 interaction scenarios. Recording and probe requests are stubbed; note files are real. The fixture's `frames` count excludes the interaction frames.

From the repository root, using an x86 Visual Studio developer prompt, Python 3 and the existing static x86 vcpkg dependencies, substitute scratch/vcpkg paths and quote paths with spaces:

```bat
mkdir <scratch>
python tools/report_check/extract_menu.py <scratch>/report_ui_under_test.inc
cl /nologo /std:c++20 /utf-8 /EHsc /MT /I. /Iui /Iframework /Ifeatures /I<scratch> /I<vcpkg>/include tools/report_check/menu_check.cpp <scratch>/widgets_recorded.cpp ui/violet_theme.cpp ui/icons.cpp ui/i18n.cpp framework/apex_util.cpp i18n/tr_menu.cpp i18n/tr_widgets.cpp /Fo<scratch>/ /Fe<scratch>/menu_check.exe /link /LIBPATH:<vcpkg>/lib imgui.lib user32.lib
<scratch>/menu_check.exe <scratch>
```

The extractor generates the Report implementation, recorder/gate slices and a temporary copy of `widgets.cpp` with only the `IconTextButton` entry point renamed. The fixture wraps that real entry point to collect button rectangles and disabled state for mouse clicks; it does not replace the drawing or translation logic. Generated files belong outside the source tree. Native PNG previews use CPU rasterization and real WIC output; preview success does not prove DX9 driver performance.

The fixture also extracts the production notice layout and validates 120 cases: four languages, three viewport widths and two font sizes. It seeds a narrow previous window and checks first-frame centering, viewport bounds and stable wrapping. Recorded BeginAdvanced preserves the real expandable widget while enabling clicks on the completed receipt sections. These checks do not validate gameplay or GPU performance.
## Restored 2.5.3 RC page

The previous `menu_check.cpp` fixture below tests the superseded guided redesign. For the restored session/capture/list/help page use `menu_253_check.cpp` with the whole block from `struct ReportState` to the Developer section extracted as `report_ui_under_test.inc`. Include all `i18n/tr_*.cpp` tables. Current cases cover four languages, two widths/font scales, idle/recording/pending save/failure and the optional post-save notes form. Capture storage is real and isolated; game APIs are inert. Also verify generic metadata for an unnamed capture and collection, and preservation of existing notes. These are native UI/storage checks, not gameplay validation.


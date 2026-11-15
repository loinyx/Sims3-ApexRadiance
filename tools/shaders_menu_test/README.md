# Shaders and Capture menu layout checks

`menu.cpp` draws Image > Shaders (tabs Look, Packs and Library, with the download bar) and Image > Capture (Screenshots and Video) with the real
ImGui, theme, icons, translations and fonts (ImGui 1.92 textures, as the game's DX9 backend). The production units are
compiled as they are: `patches/looks_patch.cpp`, `features/apex_capture.cpp` and `ui/widgets.cpp` are included by
`looks_fixture.cpp`, `capture_fixture.cpp` and `widgets_rec.cpp` after `rec.h`, which records every control's
rectangle. `extract_ui.py` copies the two pages and their helpers from `apex_gui.cpp`, `FailureText` from
`features/looks_packs.cpp` and the CPU rasterizer of `tools/report_check/menu_253_check.cpp`.

The shader compiler (`Looks::Fx`), pack downloads (`Looks::Packs`), the video encoder and the D3D9 hooks are stubbed
with fake data: a look of five runnable shaders (one of the player's with every parameter control, author groups,
`ui_text` notes and four quality settings) plus one in a pack that is not downloaded, packs Waiting, Downloading, Done
and failed, and capture settings that show every optional row.

It runs 1344 cases: 21 languages, content widths 400 and 900 units, text 100% and 200%, 16 states (shader panels,
groups and Advanced open or closed, compiling, edited values, import offer, packs ticked with the For this look group
and the needed standard effects, the download bar choosing, downloading and finished with a failure, the Library with
its removal confirmation on a pack and on a .fx file, screenshot and video idle, recording and saving), four frames
each. A case fails on an ImGui stack or error-recovery error, a missing translation (developer mode, as the menu notes
them; a text that is already a translation and the fixture's .fx author texts are skipped), a control past its window's right edge or cut off at its bottom, two controls overlapping, text
crossing a control, text over text, or text past the right edge. A self-check frame with each fault runs first.
Previews of pt and en (400 and 900, one at 200%) and of the first case of each finding are written as PPM.

    tools\shaders_menu_test\build.bat <output folder>
    <output folder>\shaders_menu.exe <output folder> "look-open-compiling pt/400/200%"

The second line prints that case's controls and text runs. CPU previews and layout checks do not establish gameplay,
GPU, input, file-picker or download behaviour, or visual parity in the game.

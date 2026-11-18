@echo off
rem Usage: tools\shaders_menu_test\live.bat <output folder>   (from the repository root): builds and opens the menu viewer
if "%~1"=="" exit /b 2
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
set OUT=%~1
if not exist "%OUT%" mkdir "%OUT%"
python tools\shaders_menu_test\extract_ui.py "%OUT%" || exit /b 1
set VI=vcpkg_installed\x86-windows-static
set T=tools\shaders_menu_test
cl /nologo /Fe:"%OUT%\ApexMenuViewer.exe" /Fo:"%OUT%\\" /std:c++20 /EHsc /utf-8 /MT /O1 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /bigobj /I"%OUT%" /I%T% /I. /Iui /Iframework /Ifeatures /Ipatches /I%VI%\include ^
  %T%\live.cpp %T%\pages.cpp %T%\looks_fixture.cpp %T%\capture_fixture.cpp %T%\widgets_rec.cpp %T%\stubs.cpp ^
  ui\violet_theme.cpp ui\icons.cpp ui\i18n.cpp ui\logo.cpp i18n\tr_*.cpp i18n\lang_*.cpp features\looks_runner.cpp features\looks_preset.cpp framework\apex_util.cpp ^
  %VI%\lib\imgui.lib user32.lib ole32.lib shell32.lib winmm.lib d3d9.lib dwmapi.lib gdi32.lib /link /SUBSYSTEM:WINDOWS > "%OUT%\live.log" 2>&1
if errorlevel 1 (findstr /i /c:"error" "%OUT%\live.log" & exit /b 1)

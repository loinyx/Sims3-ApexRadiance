@echo off
rem Usage: tools\shaders_menu_test\build.bat <output folder>   (from the repository root)
if "%~1"=="" exit /b 2
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
set OUT=%~1
if not exist "%OUT%" mkdir "%OUT%"
python tools\shaders_menu_test\extract_ui.py "%OUT%" || exit /b 1
set VI=vcpkg_installed\x86-windows-static
set T=tools\shaders_menu_test
cl /nologo /std:c++20 /EHsc /utf-8 /MT /O1 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /bigobj /I"%OUT%" /I%T% /I. /Iui /Iframework /Ifeatures /Ipatches /I%VI%\include ^
  %T%\menu.cpp %T%\pages.cpp %T%\looks_fixture.cpp %T%\capture_fixture.cpp %T%\widgets_rec.cpp %T%\stubs.cpp ^
  ui\violet_theme.cpp ui\icons.cpp ui\i18n.cpp i18n\tr_*.cpp i18n\lang_*.cpp features\looks_runner.cpp features\looks_preset.cpp features\looks_rest.cpp framework\apex_util.cpp ^
  %VI%\lib\imgui.lib user32.lib ole32.lib shell32.lib winmm.lib d3d9.lib /Fe:"%OUT%\shaders_menu.exe" /Fo:"%OUT%\\" > "%OUT%\build.log" 2>&1 || (findstr /i /c:"error" "%OUT%\build.log" & exit /b 1)
"%OUT%\shaders_menu.exe" "%OUT%"

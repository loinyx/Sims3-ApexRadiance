@echo off
rem native\remix-bridge\test-gi-pixel.cmd: offline test of the per-pixel shadow pass exports (patch 0019). No game is started.
rem Run build.cmd first: the test uses its source tree in src\ and its bridge pair in src\bridge\_output.
rem test_gi_pixel_device: a 32-bit D3D9 app through the built bridge pair draws a floor into an INTZ depth texture, sends
rem the pass with one lamp, two capsules and a static box, and checks the four outputs after GetRenderTargetData, also
rem after 100 frames and with a second set of textures.
rem Modes: off (server on system Direct3D 9: state "stock renderer", the pass export refuses), and on when the variable
rem DXVK_X64 names the d3d9.dll of native\dxvk\build.cmd (external passes and ray queries).
rem Needs what build.cmd needs, Python, and a desktop session with a Vulkan GPU. Exit code 0: all passed.
setlocal
set HERE=%~dp0
set SRC=%HERE%src
if not exist "%SRC%\bridge\_output\.trex\NvRemixBridge.exe" (echo run build.cmd first & exit /b 1)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo VS 2022 build tools not found & exit /b 1)
where meson >nul 2>&1 || (echo meson not on PATH: pip install --user meson ninja, then add its Scripts folder to PATH & exit /b 1)

call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
pushd "%SRC%\bridge" || exit /b 1
if not exist _t_x86\build.ninja (
  meson setup --buildtype release --backend ninja -Denable_tests=true _t_x86 || (popd & exit /b 1)
)
meson compile -C _t_x86 test_gi_pixel_device_x86 || (popd & exit /b 1)
popd

set UNIT=%SRC%\bridge\test\rtx\unit
set RUNS=off
if defined DXVK_X64 set RUNS=off on
for %%m in (%RUNS%) do (
  set REMIX_GI_TEST_CASE=%%m
  set REMIX_GI_TEST_DXVK=
  if "%%m"=="on" set REMIX_GI_TEST_DXVK=%DXVK_X64%
  python "%UNIT%\test_remix_api_bridge_app.py" --testExe test_gi_pixel_device_x86.exe --executablesDir "%SRC%\bridge\_t_x86\test\rtx\unit" --bridgeDir "%SRC%\bridge\_output" --artifactDir "%SRC%\bridge\_t_x86\test_artifacts\gi_pixel_device_%%m" || (echo FAILED: test_gi_pixel_device, mode %%m & exit /b 1)
)
echo per-pixel pass tests passed
exit /b 0

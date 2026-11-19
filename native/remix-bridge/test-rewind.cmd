@echo off
rem native\remix-bridge\test-rewind.cmd: offline tests of rewind mode (patch 0015). No game is started.
rem Run build.cmd first: the tests use its source tree in src\ and its bridge pair in src\bridge\_output.
rem  1. test_rewind_server: the server side on a D3D9 device (tables, deferred destroys, kept contents, render targets,
rem     device state, queries, the memory limit, the refusals). It runs on the system's d3d9.dll and, when
rem     out\package\.trex\d3d9vk_x64.dll exists (package.ps1), on that DXVK as well.
rem  2. test_rewind_device: a 32-bit D3D9 app through the built bridge pair, with the mode on (park at Present, snapshot,
rem     plan and drop, other threads kept from sending) and off (nothing published).
rem Needs what build.cmd needs, Python, and a desktop session with a GPU. Exit code 0: all passed.
setlocal
set HERE=%~dp0
set SRC=%HERE%src
if not exist "%SRC%\bridge\_output\.trex\NvRemixBridge.exe" (echo run build.cmd first & exit /b 1)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo VS 2022 build tools not found & exit /b 1)
where meson >nul 2>&1 || (echo meson not on PATH: pip install --user meson ninja, then add its Scripts folder to PATH & exit /b 1)

call :one x64 test_rewind_server_x64 || exit /b 1
call :one x86 test_rewind_device_x86 || exit /b 1

set UNIT=%SRC%\bridge\test\rtx\unit
set REWIND_TEST_D3D9=
"%SRC%\bridge\_t_x64\test\rtx\unit\test_rewind_server_x64.exe" || (echo FAILED: test_rewind_server on d3d9.dll & exit /b 1)
if exist "%HERE%out\package\.trex\d3d9vk_x64.dll" (
  set REWIND_TEST_D3D9=%HERE%out\package\.trex\d3d9vk_x64.dll
  "%SRC%\bridge\_t_x64\test\rtx\unit\test_rewind_server_x64.exe" || (echo FAILED: test_rewind_server on DXVK & exit /b 1)
  set REWIND_TEST_D3D9=
) else (
  echo skipped: test_rewind_server on DXVK, no out\package\.trex\d3d9vk_x64.dll
)
for %%m in (on off) do (
  set REMIX_REWIND_TEST_CASE=%%m
  python "%UNIT%\test_remix_api_bridge_app.py" --testExe test_rewind_device_x86.exe --executablesDir "%SRC%\bridge\_t_x86\test\rtx\unit" --bridgeDir "%SRC%\bridge\_output" --artifactDir "%SRC%\bridge\_t_x86\test_artifacts\rewind_device_%%m" || (echo FAILED: test_rewind_device, mode %%m & exit /b 1)
)
echo rewind tests passed
exit /b 0

:one
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul || exit /b 1
cd /d "%SRC%\bridge" || exit /b 1
if not exist _t_%1\build.ninja (
  meson setup --buildtype release --backend ninja -Denable_tests=true _t_%1 || exit /b 1
)
meson compile -C _t_%1 %2 || exit /b 1
endlocal
exit /b 0

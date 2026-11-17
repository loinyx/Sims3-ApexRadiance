@echo off
rem native\remix-bridge\test-async-readback.cmd: offline test of the asynchronous read-back exports (patch 0017). No game is started.
rem Run build.cmd first: the test uses its source tree in src\ and its bridge pair in src\bridge\_output.
rem test_async_readback_device: a 32-bit D3D9 app through the built bridge pair reads ColorFill patterns (16x16 and
rem 256x256) back through GetRenderTargetData and through RemixBridge_ReadRenderTargetAsync / RemixBridge_PollReadback
rem and compares the bytes; it also fills all four slots, cancels one and checks the refusals. Mode off checks E_NOTIMPL.
rem Needs what build.cmd needs, Python, and a desktop session with a GPU. Exit code 0: all passed.
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
meson compile -C _t_x86 test_async_readback_device_x86 || (popd & exit /b 1)
popd

set UNIT=%SRC%\bridge\test\rtx\unit
for %%m in (on off) do (
  set REMIX_ASYNC_READBACK_TEST_CASE=%%m
  python "%UNIT%\test_remix_api_bridge_app.py" --testExe test_async_readback_device_x86.exe --executablesDir "%SRC%\bridge\_t_x86\test\rtx\unit" --bridgeDir "%SRC%\bridge\_output" --artifactDir "%SRC%\bridge\_t_x86\test_artifacts\async_readback_device_%%m" || (echo FAILED: test_async_readback_device, mode %%m & exit /b 1)
)
echo async read-back tests passed
exit /b 0

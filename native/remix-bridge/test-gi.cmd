@echo off
rem native\remix-bridge\test-gi.cmd: offline test of the GI server exports (patch 0018). No game is started.
rem Run build.cmd first: the test uses its source tree in src\ and its bridge pair in src\bridge\_output.
rem test_gi_device: a 32-bit D3D9 app through the built bridge pair has the server's GI worker fill DEFAULT-pool
rem render target textures (A16B16G16R16F and A8R8G8B8, up to 2 MB) and checks every texel after GetRenderTargetData;
rem it also checks a superseded solve, a cancel and the refusals, and prints the worker and upload times.
rem The solve: tools\gi_server_test first solves its synthetic lot offline, path-traces a reference of it and writes
rem both with the scene into one file (%TEMP%\ApexGiServerTest\gi_scene.bin). The test app submits that scene through
rem the bridge, solves every lamp's layers in the server and compares them with the offline solve by texel and with
rem the reference by room mean; then a lamp change, an 8-bit target, a stale scene ticket and a refused blob.
rem Modes: on (server on system Direct3D 9, ray queries when the GPU has them), compute (the same without rays),
rem off (E_NOTIMPL), stock (level none), and on and compute again with DXVK when the variable DXVK_X64 names a
rem d3d9vk_x64.dll (DXVK 3.1.1, as in the package).
rem Needs what build.cmd needs, Python, PowerShell 7 and a desktop session with a Vulkan GPU. Exit code 0: all passed.
setlocal
set HERE=%~dp0
set SRC=%HERE%src
if not exist "%SRC%\bridge\_output\.trex\NvRemixBridge.exe" (echo run build.cmd first & exit /b 1)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo VS 2022 build tools not found & exit /b 1)
where meson >nul 2>&1 || (echo meson not on PATH: pip install --user meson ninja, then add its Scripts folder to PATH & exit /b 1)

set REMIX_GI_TEST_SCENE=%TEMP%\ApexGiServerTest\gi_scene.bin
pwsh -NoProfile -Command "& '%HERE%..\..\tools\gi_server_test\run.ps1' -TestArgs '--dump','%REMIX_GI_TEST_SCENE%'" || (echo FAILED: the offline solve & exit /b 1)
rem The lots of 60, 120 and 200 lamps (rebake_check.cpp --dump), each lamp scoped to its room grown by one room: modes
rem lots (list-scoped dispatch) and lots-full (server.giListDispatch = False) solve every lamp and print the GPU time a layer.
set LOTS=%TEMP%\ApexGiServerTest\lots
if not exist "%LOTS%" mkdir "%LOTS%"
pwsh -NoProfile -Command "& '%HERE%..\..\tools\gi_server_test\run.ps1' -Rebake -TestArgs '--no-cpu','--dump','%LOTS%'" || (echo FAILED: the lots & exit /b 1)
set REMIX_GI_TEST_LOTS=%LOTS%\gi_lot60.bin;%LOTS%\gi_lot120.bin;%LOTS%\gi_lot200.bin

call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
rem The listed gather batches against a cost model (gi_list_batch.h, no GPU).
pushd "%TEMP%\ApexGiServerTest" || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /I "%HERE%server" "%HERE%..\..\tools\gi_server_test\list_batch_check.cpp" >nul || (popd & echo FAILED: list_batch_check did not compile & exit /b 1)
.\list_batch_check.exe || (popd & echo FAILED: list_batch_check & exit /b 1)
popd
pushd "%SRC%\bridge" || exit /b 1
if not exist _t_x86\build.ninja (
  meson setup --buildtype release --backend ninja -Denable_tests=true _t_x86 || (popd & exit /b 1)
)
meson compile -C _t_x86 test_gi_device_x86 || (popd & exit /b 1)
popd

set UNIT=%SRC%\bridge\test\rtx\unit
set RUNS=on compute off stock lots lots-full
if defined DXVK_X64 set RUNS=on compute off stock lots lots-full dxvk dxvk-compute
for %%m in (%RUNS%) do (
  set REMIX_GI_TEST_CASE=%%m
  set REMIX_GI_TEST_DXVK=
  if "%%m"=="dxvk" (
    set REMIX_GI_TEST_CASE=on
    set REMIX_GI_TEST_DXVK=%DXVK_X64%
  )
  if "%%m"=="dxvk-compute" (
    set REMIX_GI_TEST_CASE=compute
    set REMIX_GI_TEST_DXVK=%DXVK_X64%
  )
  python "%UNIT%\test_remix_api_bridge_app.py" --testExe test_gi_device_x86.exe --executablesDir "%SRC%\bridge\_t_x86\test\rtx\unit" --bridgeDir "%SRC%\bridge\_output" --artifactDir "%SRC%\bridge\_t_x86\test_artifacts\gi_device_%%m" --timeout 240 || (echo FAILED: test_gi_device, mode %%m & exit /b 1)
)
echo GI server tests passed
exit /b 0

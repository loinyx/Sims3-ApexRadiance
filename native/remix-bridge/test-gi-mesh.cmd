@echo off
rem native\remix-bridge\test-gi-mesh.cmd <game folder> <objects file>: offline test of the mesh casters (patch 0020). No game is started.
rem Run build.cmd first: the test uses its source tree in src\ and its bridge pair in src\bridge\_output. The objects file is
rem meshes_check.cpp's --objects output (tools\gi_server_test\run.ps1 -Mesh -Game <folder> -Bridge writes it and calls this).
rem test_gi_mesh_device: a 32-bit D3D9 app through the built bridge pair sends a chair, an unknown LITE and a removed object
rem with RemixBridge_GiObjects and checks the server's counters (index ready, one model resolved, one missing). With DXVK_X64
rem naming the d3d9.dll of native\dxvk\build.cmd it then runs the per-pixel pass with and without the chair and compares the
rem chair's shadow on the floor with a CPU ray cast of the same rays.
rem It also reads the chair's caster mesh back with RemixBridge_GiMeshFetch / RemixBridge_GiMeshRead (patch 0022), which must equal
rem the objects file, and times 50 fetches.
rem Modes: counters (server on system Direct3D 9), on (server on DXVK_X64). Exit code 0: all passed.
setlocal
set HERE=%~dp0
set SRC=%HERE%src
if "%~2"=="" (echo usage: test-gi-mesh.cmd ^<game folder^> ^<objects file^> & exit /b 2)
if not exist "%SRC%\bridge\_output\.trex\NvRemixBridge.exe" (echo run build.cmd first & exit /b 1)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo VS 2022 build tools not found & exit /b 1)
where meson >nul 2>&1 || (echo meson not on PATH: pip install --user meson ninja, then add its Scripts folder to PATH & exit /b 1)

call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
pushd "%SRC%\bridge" || exit /b 1
if not exist _t_x86\build.ninja (
  meson setup --buildtype release --backend ninja -Denable_tests=true _t_x86 || (popd & exit /b 1)
)
meson compile -C _t_x86 test_gi_mesh_device_x86 || (popd & exit /b 1)
popd

set REMIX_GI_MESH_GAME=%~1
set REMIX_GI_MESH_OBJECTS=%~f2
set UNIT=%SRC%\bridge\test\rtx\unit
set RUNS=counters
if defined DXVK_X64 set RUNS=counters on
for %%m in (%RUNS%) do (
  set REMIX_GI_TEST_CASE=%%m
  set REMIX_GI_TEST_DXVK=
  if "%%m"=="on" set REMIX_GI_TEST_DXVK=%DXVK_X64%
  python "%UNIT%\test_remix_api_bridge_app.py" --testExe test_gi_mesh_device_x86.exe --executablesDir "%SRC%\bridge\_t_x86\test\rtx\unit" --bridgeDir "%SRC%\bridge\_output" --artifactDir "%SRC%\bridge\_t_x86\test_artifacts\gi_mesh_device_%%m" --timeout 300 || (echo FAILED: test_gi_mesh_device, mode %%m & exit /b 1)
)
echo mesh caster tests passed
exit /b 0

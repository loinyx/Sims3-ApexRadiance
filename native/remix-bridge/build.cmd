@echo off
rem native\remix-bridge\build.cmd: fetches NVIDIA's RTX Remix bridge (dxvk-remix\bridge, MIT) at the pinned commit, applies
rem patches\*.patch and builds the x86 client (d3d9.dll) and the x64 server (.trex\NvRemixBridge.exe), release.
rem Needs git, curl, tar, VS 2022 C++ build tools (x86 + x64) and meson + ninja on PATH (pip install --user meson ninja).
rem Output: out\d3d9.dll and out\.trex\NvRemixBridge.exe. The source goes to src\ (git-ignored); delete src\ to refetch.
setlocal
set UPSTREAM=https://github.com/NVIDIAGameWorks/dxvk-remix.git
set COMMIT=f35b86a494712eba72177ddabe62a48ff4cb6e9c
set DETOURS=https://github.com/microsoft/Detours.git
set DETOURS_COMMIT=ea6c4ae7f3f1b1772b8a7cda4199230b932f5a50
rem the GI worker (patch 0018): Vulkan headers, the volk loader and the glslang compiler, at pinned versions
set VULKAN_SDK_TAG=vulkan-sdk-1.4.350.1
set GLSLANG_VERSION=16.6.0
set HERE=%~dp0
set SRC=%HERE%src
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo VS 2022 build tools not found & exit /b 1)
where meson >nul 2>&1 || (echo meson not on PATH: pip install --user meson ninja, then add its Scripts folder to PATH & exit /b 1)

if exist "%SRC%\.git" goto build
rem only bridge\ and the shared headers it includes (public\, src\util\) are needed, not the whole runtime
git clone -q --filter=blob:none --no-checkout %UPSTREAM% "%SRC%" || exit /b 1
git -C "%SRC%" sparse-checkout set bridge public src/util || exit /b 1
git -C "%SRC%" checkout -q %COMMIT% || exit /b 1
rem the bridge links Detours from the submodule path; the sparse checkout leaves submodules out
git clone -q --filter=blob:none %DETOURS% "%SRC%\submodules\Detours" || exit /b 1
git -C "%SRC%\submodules\Detours" checkout -q %DETOURS_COMMIT% || exit /b 1
git clone -q --depth 1 --branch %VULKAN_SDK_TAG% https://github.com/KhronosGroup/Vulkan-Headers.git "%SRC%\submodules\Vulkan-Headers" || exit /b 1
git clone -q --depth 1 --branch %VULKAN_SDK_TAG% https://github.com/zeux/volk.git "%SRC%\submodules\volk" || exit /b 1
mkdir "%SRC%\submodules\glslang" || exit /b 1
curl -sSfL -o "%SRC%\glslang.zip" https://github.com/KhronosGroup/glslang/releases/download/%GLSLANG_VERSION%/glslang-%GLSLANG_VERSION%-windows-x86_64-release.zip || exit /b 1
"%SystemRoot%\System32\tar.exe" -xf "%SRC%\glslang.zip" -C "%SRC%\submodules\glslang" || exit /b 1
del "%SRC%\glslang.zip"
for %%p in ("%HERE%patches\*.patch") do (
  git -C "%SRC%" apply --whitespace=nowarn "%%~fp" || (echo patch %%~nxp did not apply & exit /b 1)
  echo applied %%~nxp
)

:build
rem the per-pixel shadow pass (patch 0019) compiles three files of this repository; copied on every build so edits arrive
copy /y "%HERE%gi\gi_pixel_block.h" "%SRC%\bridge\src\util\" >nul || exit /b 1
copy /y "%HERE%gi\gi_pixel_pass.h" "%SRC%\bridge\src\server\" >nul || exit /b 1
copy /y "%HERE%..\..\tools\gi_server_test\pixel_shadow.comp" "%SRC%\bridge\src\server\" >nul || exit /b 1
for %%A in (x86 x64) do (
  echo == %%A
  call :one %%A || exit /b 1
)
if not exist "%HERE%out\.trex" mkdir "%HERE%out\.trex"
copy /y "%SRC%\bridge\_output\d3d9.dll" "%HERE%out\" >nul || exit /b 1
copy /y "%SRC%\bridge\_output\.trex\NvRemixBridge.exe" "%HERE%out\.trex\" >nul || exit /b 1
echo built: %HERE%out\d3d9.dll and %HERE%out\.trex\NvRemixBridge.exe
exit /b 0

:one
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul || exit /b 1
cd /d "%SRC%\bridge" || exit /b 1
if not exist _b_%1\build.ninja (
  meson setup --buildtype release --backend ninja _b_%1 || exit /b 1
)
meson compile -C _b_%1 || exit /b 1
endlocal
exit /b 0

@echo off
rem native\dxvk\build.cmd: fetches DXVK (zlib, https://github.com/doitsujin/dxvk) at the pinned v3.1.1 commit, applies
rem patches\*.patch and builds the x64 d3d9.dll the bridge server renders through, release, MSVC, static CRT.
rem Needs git, VS 2022 C++ build tools (x64) and meson + ninja on PATH (pip install --user meson ninja).
rem glslang (DXVK's shaders) is fetched once into tools\ from the pinned glslang release, checked by SHA-256.
rem Output: out\d3d9.dll. The source goes to src\ (git-ignored); delete src\ to refetch, delete src\_b_x64 to reconfigure.
setlocal
set UPSTREAM=https://github.com/doitsujin/dxvk.git
set TAG=v3.1.1
set COMMIT=b1a1c99ab52b687cf950d62c88bc2fa316b41663
set GLSLANG_VER=16.6.0
set GLSLANG_SHA=82BF434E69B9BB4829DE7E2B4BC2C5E7A7861E53D66CF75E5CC70F5F694A8D9B
set HERE=%~dp0
set SRC=%HERE%src
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo VS 2022 build tools not found & exit /b 1)
where meson >nul 2>&1 || (echo meson not on PATH: pip install --user meson ninja, then add its Scripts folder to PATH & exit /b 1)

if exist "%HERE%tools\bin\glslang.exe" goto fetched
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; $z='%HERE%tools\glslang.zip'; New-Item -ItemType Directory -Force '%HERE%tools' | Out-Null;" ^
  "Invoke-WebRequest 'https://github.com/KhronosGroup/glslang/releases/download/%GLSLANG_VER%/glslang-%GLSLANG_VER%-windows-x86_64-release.zip' -OutFile $z -UseBasicParsing;" ^
  "if ((Get-FileHash $z -Algorithm SHA256).Hash -ne '%GLSLANG_SHA%') { Remove-Item $z; throw 'glslang archive hash mismatch' };" ^
  "Expand-Archive -Force $z '%HERE%tools'; Remove-Item $z" || exit /b 1
:fetched
set PATH=%HERE%tools\bin;%PATH%

if exist "%SRC%\.git" goto build
git clone -q -c advice.detachedHead=false --branch %TAG% --depth 1 --recurse-submodules --shallow-submodules %UPSTREAM% "%SRC%" || exit /b 1
for /f %%c in ('git -C "%SRC%" rev-parse HEAD') do if not "%%c"=="%COMMIT%" (echo %TAG% is %%c, expected %COMMIT% & exit /b 1)
for %%p in ("%HERE%patches\*.patch") do (
  git -C "%SRC%" apply --whitespace=nowarn "%%~fp" || (echo patch %%~nxp did not apply & exit /b 1)
  echo applied %%~nxp
)

:build
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
cd /d "%SRC%" || exit /b 1
rem only d3d9; static CRT so players need no VC++ redistributable
if not exist _b_x64\build.ninja (
  meson setup --buildtype release --backend ninja -Db_vscrt=mt -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false _b_x64 || exit /b 1
)
meson compile -C _b_x64 || exit /b 1
if not exist "%HERE%out" mkdir "%HERE%out"
copy /y "%SRC%\_b_x64\src\d3d9\d3d9.dll" "%HERE%out\" >nul || exit /b 1
echo built: %HERE%out\d3d9.dll
exit /b 0

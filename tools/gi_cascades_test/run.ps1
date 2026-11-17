param([string[]]$TestArgs = @(), [string]$OutDir = (Join-Path $env:TEMP 'ApexGiCascadesTest'))
# Real-time GI's radiance cascades as a compute shader, offline on a Vulkan device (check.cpp, cascades.comp) against the CPU
# (features/plan_cascades.h). Read-only: prints to stdout; the compiler's outputs and the three build inputs below stay in
# -OutDir (tools/gi_server_test's folder holds the same inputs). -TestArgs: passed on (--device <text>).
# Build inputs, fetched once at pinned versions when -OutDir does not hold them (no installed Vulkan SDK is needed; the
# Vulkan loader, vulkan-1.dll, comes with the graphics driver): the Vulkan headers, the volk loader (two files) and the
# glslang compiler, which turns cascades.comp into SPIR-V headers at build time.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$sdk = 'vulkan-sdk-1.4.350.1'
$glslangVersion = '16.6.0'
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
function Get-Pinned([string]$Url, [string]$Name) {
    $dir = Join-Path $OutDir $Name
    if (Test-Path -LiteralPath $dir) { return $dir }
    $zip = Join-Path $OutDir ($Name + '.zip')
    Invoke-WebRequest -Uri $Url -OutFile $zip -TimeoutSec 120
    Expand-Archive -LiteralPath $zip -DestinationPath ($dir + '.tmp') -Force
    Remove-Item -LiteralPath $zip
    Rename-Item -LiteralPath ($dir + '.tmp') -NewName $Name
    return $dir
}
$headers = Join-Path (Get-Pinned "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$sdk.zip" "headers-$sdk") "Vulkan-Headers-$sdk/include"
$volk = Join-Path (Get-Pinned "https://github.com/zeux/volk/archive/refs/tags/$sdk.zip" "volk-$sdk") "volk-$sdk"
$glslang = Get-ChildItem -LiteralPath (Get-Pinned "https://github.com/KhronosGroup/glslang/releases/download/$glslangVersion/glslang-$glslangVersion-windows-x86_64-release.zip" "glslang-$glslangVersion") -Recurse -Filter 'glslang.exe' | Select-Object -First 1
if (-not $glslang) { throw 'glslang.exe not found in the fetched archive' }

# The x64 toolchain (the bridge server is a 64-bit process; tools/terrain_lighting_test/compiler_env.ps1 sets up x86)
$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not ($compiler -and $compiler.Source -match '[\\/]bin[\\/]Host(?:x64|x86)[\\/]x64[\\/]cl\.exe$' -and $env:INCLUDE -and $env:LIB)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'No installed x64 MSVC toolchain found' }
    $environmentLines = & cmd.exe /d /c ('call "' + (Join-Path $installation 'Common7/Tools/VsDevCmd.bat') + '" -no_logo -arch=x64 -host_arch=x64 >nul && set')
    if ($LASTEXITCODE -ne 0) { throw 'Compiler initialization failed' }
    foreach ($line in $environmentLines) {
        if ($line -match '^(PATH|INCLUDE|LIB|LIBPATH)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
}
Push-Location $OutDir
try {
    foreach ($v in @(@('cascade', 0), @('gather', 1))) { # name, pass
        & $glslang.FullName -V --quiet "-DPASS=$($v[1])" --target-env vulkan1.1 --vn $v[0] -o (Join-Path $OutDir ($v[0] + '.h')) (Join-Path $PSScriptRoot 'cascades.comp')
        if ($LASTEXITCODE -ne 0) { throw "cascades.comp ($($v[0])) did not compile" }
    }
    $exe = Join-Path $OutDir 'gi_cascades_test.exe'
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX /DVK_USE_PLATFORM_WIN32_KHR ('/I' + $repo) ('/I' + $OutDir) ('/I' + $headers) ('/I' + $volk) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Server cascades test did not compile' }
    & $exe @TestArgs
    if ($LASTEXITCODE -ne 0) { throw 'Server cascades test failed' }
} finally { Pop-Location }

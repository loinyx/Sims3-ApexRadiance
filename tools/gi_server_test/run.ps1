param([string[]]$TestArgs = @(), [string]$OutDir = (Join-Path $env:TEMP 'ApexGiServerTest'))
# The server tier's texel solve, offline on a Vulkan device (check.cpp, solve.comp). Read-only: prints to stdout; the
# compiler's outputs and the three build inputs below stay in -OutDir. -TestArgs: passed on (--no-rays, --device <text>,
# --density <n>, --rays <n>).
# Build inputs, fetched once at pinned versions when -OutDir does not hold them (no installed Vulkan SDK is needed; the
# Vulkan loader, vulkan-1.dll, comes with the graphics driver): the Vulkan headers, the volk loader (two files) and the
# glslang compiler, which turns solve.comp into SPIR-V headers at build time.
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
    # name, pass, ray queries (SPIR-V 1.4 for the ray-query extension; the compute back end stays at Vulkan 1.1)
    foreach ($v in @(@('links_rq', 0, $true), @('direct_rq', 1, $true), @('links_sw', 0, $false), @('direct_sw', 1, $false), @('bounce', 2, $false))) {
        $defs = @("-DPASS=$($v[1])") + $(if ($v[2]) { @('-DRAYQUERY', '--target-env', 'vulkan1.2') } else { @('--target-env', 'vulkan1.1') })
        & $glslang.FullName -V --quiet @defs --vn $v[0] -o (Join-Path $OutDir ($v[0] + '.h')) (Join-Path $PSScriptRoot 'solve.comp')
        if ($LASTEXITCODE -ne 0) { throw "solve.comp ($($v[0])) did not compile" }
    }
    $exe = Join-Path $OutDir 'gi_server_test.exe'
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX /DVK_USE_PLATFORM_WIN32_KHR ('/I' + $repo) ('/I' + $OutDir) ('/I' + $headers) ('/I' + $volk) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Server texel solve test did not compile' }
    & $exe @TestArgs
    if ($LASTEXITCODE -ne 0) { throw 'Server texel solve test failed' }
} finally { Pop-Location }

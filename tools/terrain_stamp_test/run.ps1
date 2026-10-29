param([Parameter(Mandatory = $true)][string]$Precomp, [string]$OutDir = (Join-Path $env:TEMP 'ApexTerrainStampTest'))
# Terrain lamp stamp, offline (read-only: prints to stdout; the compiler's outputs stay in -OutDir). -Precomp: the game's
# Bin\Shaders_Win32.precomp, read for the bake shaders (technique StaticTerrainLightmap); nothing of it is written.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'terrain_stamp_test.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Terrain stamp test did not compile' }
    & $exe (Resolve-Path -LiteralPath $Precomp).Path
    if ($LASTEXITCODE -ne 0) { throw 'Terrain stamp test failed' }
    # the dragged-lamp state machine (features/terrain_stamps.cpp) against a simulated terrain
    $state = Join-Path $OutDir 'terrain_stamp_state.exe'
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /DNOMINMAX ('/I' + $repo) ('/I' + (Join-Path $repo 'framework')) ('/Fe' + $state) (Join-Path $PSScriptRoot 'state_check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Terrain stamp state test did not compile' }
    & $state
    if ($LASTEXITCODE -ne 0) { throw 'Terrain stamp state test failed' }
} finally { Pop-Location }

param([string]$Precomp = '', [string]$OutDir = (Join-Path $env:TEMP 'ApexGiIndirectTest'))
# Real-time GI S2-05 (the lighting reading the irradiance atlas), offline check (read-only: prints to stdout; the compiler's
# outputs stay in -OutDir). -Precomp: the game's Shaders_Win32.precomp, for the checks over the game's room-lit shaders.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'gi_indirect_test.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp') (Join-Path $repo 'features/gi_compose.cpp') (Join-Path $repo 'features/gi_sched.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'GI indirect check did not compile' }
    $checkArgs = @()
    if ($Precomp) { $checkArgs += (Resolve-Path -LiteralPath $Precomp).Path }
    & $exe @checkArgs
    if ($LASTEXITCODE -ne 0) { throw 'GI indirect check failed' }
} finally { Pop-Location }

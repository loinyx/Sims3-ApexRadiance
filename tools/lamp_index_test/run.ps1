param([string]$Precomp = '', [string]$OutDir = (Join-Path $env:TEMP 'ApexLampIndexTest'))
# Lamp index cells (real-time GI S3-02), offline check (read-only: prints to stdout; the compiler's outputs stay in -OutDir).
# -Precomp: the game's Shaders_Win32.precomp, for the checks over the game's object shaders.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'lamp_index_test.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Lamp index check did not compile' }
    $checkArgs = @((Join-Path $PSScriptRoot 'lamp_index.hlsl'))
    if ($Precomp) { $checkArgs += (Resolve-Path -LiteralPath $Precomp).Path }
    & $exe @checkArgs
    if ($LASTEXITCODE -ne 0) { throw 'Lamp index check failed' }
} finally { Pop-Location }

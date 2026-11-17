param([string]$OutDir = (Join-Path $env:TEMP 'ApexAoOptionsCheck'))
# Ambient Occlusion options, offline check (read-only: prints to stdout; the compiler's outputs stay in $OutDir)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'ao_options_check.exe'
Push-Location $repo
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT ('/Fe' + $exe) ('/Fo' + $OutDir + '\') (Join-Path $PSScriptRoot 'check.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'AO options check did not compile' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'AO options check failed' }
} finally { Pop-Location }

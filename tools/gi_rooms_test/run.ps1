param([string]$OutDir = (Join-Path $env:TEMP 'ApexGiRoomsTest'))
# Real-time GI room ids and wall-aware upsample (features/gi_rooms.h), offline. Read-only: the compiler's outputs stay in -OutDir.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX ('/I' + $repo) ('/I' + (Join-Path $repo 'features')) /Fegi_rooms_test.exe (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'GI rooms test did not compile' }
    & '.\gi_rooms_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'GI rooms test failed' }
} finally { Pop-Location }

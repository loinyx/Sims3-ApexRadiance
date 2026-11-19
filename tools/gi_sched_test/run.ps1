param([string]$OutDir = (Join-Path $env:TEMP 'ApexGiSchedTest'))
# GI update scheduler (real-time GI S2-01), offline check (read-only: prints to stdout; the compiler's outputs stay in -OutDir).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'gi_sched_test.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /O2 /MT /DNOMINMAX ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/gi_sched.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'GI scheduler check did not compile' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'GI scheduler check failed' }
} finally { Pop-Location }

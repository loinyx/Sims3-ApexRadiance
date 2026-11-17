param([string]$OutDir = (Join-Path $env:TEMP 'ApexPlanFieldTest'))
# Real-time GI's wall-plan visibility field (features/plan_field.h), offline and CPU only: plan_field_check.h's cases
# on synthetic stories. Read-only: prints to stdout; the compiler's outputs stay in -OutDir.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'plan_field_test.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'plan_field_main.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Plan field test did not compile' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Plan field test failed' }
} finally { Pop-Location }

param([string]$OutDir = (Join-Path $env:TEMP 'ApexPlanCascadesTest'))
# Real-time GI's radiance cascades over the wall-plan field (features/plan_cascades.h), offline and CPU only:
# plan_cascades_case.h's checks on synthetic stories. Read-only: prints to stdout; the compiler's outputs stay in -OutDir.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'plan_cascades_test.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'plan_cascades_main.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Plan cascades test did not compile' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Plan cascades test failed' }
} finally { Pop-Location }

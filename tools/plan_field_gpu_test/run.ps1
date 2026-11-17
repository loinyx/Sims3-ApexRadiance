param([string]$OutDir = (Join-Path $env:TEMP 'ApexPlanFieldGpu'))
# Real-time GI's wall-plan visibility field on the GPU: the ps_3_0 passes (shaders/plan_field_hlsl.h) against the CPU
# (features/plan_field.h). Needs a D3D9 GPU; read-only: writes only the compiler outputs to -OutDir. To check DXVK, place
# its 32-bit d3d9.dll next to plan_field_gpu.exe in -OutDir and run the exe again.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX ('/I' + $repo) /Feplan_field_gpu.exe (Join-Path $PSScriptRoot 'check.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Plan field GPU check did not compile' }
    & '.\plan_field_gpu.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Plan field GPU checks failed' }
} finally { Pop-Location }

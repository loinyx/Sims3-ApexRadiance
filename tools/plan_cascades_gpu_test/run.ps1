param([string]$OutDir = (Join-Path $env:TEMP 'ApexPlanCascadesGpu'))
# Real-time GI's radiance cascades on the GPU: the ps_3_0 passes (shaders/plan_cascades_hlsl.h) against the CPU
# (features/plan_cascades.h). Needs a D3D9 GPU; read-only: writes only the compiler outputs to -OutDir. To check DXVK, place
# its 32-bit d3d9.dll next to plan_cascades_gpu.exe in -OutDir and run the exe again.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX ('/I' + $repo) /Feplan_cascades_gpu.exe (Join-Path $PSScriptRoot 'check.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Plan cascades GPU check did not compile' }
    & '.\plan_cascades_gpu.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Plan cascades GPU checks failed' }
} finally { Pop-Location }

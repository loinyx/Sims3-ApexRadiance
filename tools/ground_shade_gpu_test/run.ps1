param([string]$OutDir=(Join-Path $env:TEMP 'ApexGroundShadeGpu'))
# Walls shade lamp light near the ground on the GPU: the production engine (features/ground_shade_gpu.cpp) and shaders
# (shaders/ground_shade_hlsl.h) against the CPU (features/ground_lamp_shadow.h). Needs a D3D9 GPU; read-only: writes only the
# compiler outputs to -OutDir. To check DXVK, place the game's d3d9.dll next to ground_shade_gpu.exe in -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /Oi /MT /DNOMINMAX ('/I'+$repo) ('/I'+(Join-Path $repo 'features')) /Feground_shade_gpu.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/ground_shade_gpu.cpp') d3d9.lib d3dcompiler.lib user32.lib
 if($LASTEXITCODE -ne 0){throw 'Ground shade GPU check compilation failed'}
 & '.\ground_shade_gpu.exe'
 if($LASTEXITCODE -ne 0){throw 'Ground shade GPU checks failed'}
} finally {Pop-Location}

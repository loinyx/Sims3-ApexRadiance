param([string]$OutDir=(Join-Path $env:TEMP 'ApexGBufferMotionCheck'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
# The passes' HLSL, taken from the production sources so the check compiles what the game compiles. A source split into
# adjacent literals (one literal is limited to 16 KB) is joined; separate sources are separate blocks
. (Join-Path $repo 'tools/gbuffer_check/hlsl_blocks.ps1')
$gb=Get-HlslBlocks (Join-Path $repo 'patches/gbuffer_patch.cpp')
if($gb.Count -lt 2){throw 'HLSL blocks missing in gbuffer_patch.cpp (the pass, then the temporal AA)'}
$hlsl=Join-Path $OutDir 'gbuffer.hlsl'
Set-Content -LiteralPath $hlsl -Value $gb[0] -Encoding ascii
$taa=Join-Path $OutDir 'taa.hlsl'
Set-Content -LiteralPath $taa -Value $gb[1] -Encoding ascii
# Ambient Occlusion's HLSL (its G-buffer normal and temporal passes, GB-04)
$ao=Get-HlslBlocks (Join-Path $repo 'patches/ambient_occlusion_patch.cpp')
if($ao.Count -lt 1){throw 'HLSL block missing in ambient_occlusion_patch.cpp'}
$aoHlsl=Join-Path $OutDir 'ambient_occlusion.hlsl'
Set-Content -LiteralPath $aoHlsl -Value $ao[0] -Encoding ascii
# The movers' pixel shader (GB-05)
$mv=Get-HlslBlocks (Join-Path $repo 'features/gbuffer_movers.cpp')
if($mv.Count -lt 1){throw 'HLSL block missing in gbuffer_movers.cpp'}
$movers=Join-Path $OutDir 'movers.hlsl'
Set-Content -LiteralPath $movers -Value $mv[0] -Encoding ascii
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT ('/I'+(Join-Path $repo 'features')) /Fegbuffer_motion_check.exe (Join-Path $PSScriptRoot 'gbuffer_motion_check.cpp') /link d3dcompiler.lib
 if($LASTEXITCODE){throw 'compilation failed'}
 & (Join-Path $OutDir 'gbuffer_motion_check.exe') $hlsl $aoHlsl $taa $movers
 if($LASTEXITCODE){throw 'check failed'}
} finally {Pop-Location}

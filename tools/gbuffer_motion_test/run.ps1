param([string]$OutDir=(Join-Path $env:TEMP 'ApexGBufferMotionCheck'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
# The pass's HLSL, taken from the production source so the check compiles what the game compiles
$source=Get-Content -LiteralPath (Join-Path $repo 'patches/gbuffer_patch.cpp') -Raw
$m=[regex]::Match($source,'R"HLSL\((.*?)\)HLSL"',[System.Text.RegularExpressions.RegexOptions]::Singleline)
if(-not $m.Success){throw 'HLSL block missing in gbuffer_patch.cpp'}
$hlsl=Join-Path $OutDir 'gbuffer.hlsl'
Set-Content -LiteralPath $hlsl -Value $m.Groups[1].Value -Encoding ascii
# Ambient Occlusion's HLSL (its G-buffer normal and temporal passes, GB-04)
$ao=[regex]::Match((Get-Content -LiteralPath (Join-Path $repo 'patches/ambient_occlusion_patch.cpp') -Raw),'R"HLSL\((.*?)\)HLSL"',[System.Text.RegularExpressions.RegexOptions]::Singleline)
if(-not $ao.Success){throw 'HLSL block missing in ambient_occlusion_patch.cpp'}
$aoHlsl=Join-Path $OutDir 'ambient_occlusion.hlsl'
Set-Content -LiteralPath $aoHlsl -Value $ao.Groups[1].Value -Encoding ascii
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT ('/I'+(Join-Path $repo 'features')) /Fegbuffer_motion_check.exe (Join-Path $PSScriptRoot 'gbuffer_motion_check.cpp') /link d3dcompiler.lib
 if($LASTEXITCODE){throw 'compilation failed'}
 & (Join-Path $OutDir 'gbuffer_motion_check.exe') $hlsl $aoHlsl
 if($LASTEXITCODE){throw 'check failed'}
} finally {Pop-Location}

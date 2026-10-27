param([string]$OutDir=(Join-Path $env:TEMP 'ApexSceneTargetChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /fp:precise ('/I'+(Join-Path $repo 'framework')) '/Fescene_target_check.exe' (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE){throw 'scene_target_check compilation failed'}
 & (Join-Path $OutDir 'scene_target_check.exe')
 if($LASTEXITCODE){throw 'scene_target_check failed'}
} finally {Pop-Location}

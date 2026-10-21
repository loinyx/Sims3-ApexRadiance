param([string]$OutDir=(Join-Path $env:TEMP 'ApexLivenessPruneChecks'))
# Pruning of the per-manager caches by liveness (features/liveness_prune.h). Read-only:
# writes only the compiler outputs to -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) /Feliveness_prune_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Liveness prune check compilation failed'}
 & '.\liveness_prune_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Liveness prune checks failed'}
} finally {Pop-Location}

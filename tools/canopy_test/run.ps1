param([string]$OutDir=(Join-Path $env:TEMP 'ApexCanopyChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) ('/I'+(Join-Path $repo 'features')) /Fecanopy_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Canopy check compilation failed'}
 & '.\canopy_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Canopy checks failed'}
} finally {Pop-Location}

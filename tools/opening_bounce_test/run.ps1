param([string]$OutDir=(Join-Path $env:TEMP 'ApexOpeningBounceChecks'))
# Bounce light through floor openings (features/opening_bounce.h): the share of the other story room's ambient. Read-only:
# writes only the compiler outputs to -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) /Feopening_bounce_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Opening bounce check compilation failed'}
 & '.\opening_bounce_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Opening bounce checks failed'}
} finally {Pop-Location}

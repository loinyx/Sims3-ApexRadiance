param([string]$OutDir=(Join-Path $env:TEMP 'ApexStairwellBounceChecks'))
# The second bounce through a stairwell opening (features/stairwell_bounce.h). Read-only:
# writes only the compiler outputs to -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) /Festairwell_bounce_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Stairwell bounce check compilation failed'}
 & '.\stairwell_bounce_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Stairwell bounce checks failed'}
} finally {Pop-Location}

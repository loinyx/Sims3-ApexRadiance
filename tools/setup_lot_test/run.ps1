param([string]$OutDir=(Join-Path $env:TEMP 'ApexSetupLotChecks'))
# Lot capture test offline checks (check.cpp, features/setup_lot_policy.h).
# The exe name avoids "setup": Windows treats such a name as an installer and asks for elevation.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+(Join-Path $repo 'features')) /Felot_capture_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Lot capture test compilation failed'}
 & '.\lot_capture_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Lot capture checks failed'}
} finally {Pop-Location}

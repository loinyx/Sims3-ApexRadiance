param([string]$OutDir=(Join-Path $env:TEMP 'ApexRoomSolveShareChecks'))
# Shared room solve time offline checks (check.cpp): the decision and a replay of the lot lighting update over a pan.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+(Join-Path $repo 'features')) /Feroom_solve_share_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Shared room solve time test compilation failed'}
 & '.\room_solve_share_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Shared room solve time checks failed'}
} finally {Pop-Location}

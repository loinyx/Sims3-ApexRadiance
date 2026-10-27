param([string]$OutDir=(Join-Path $env:TEMP 'ApexRoomSolveCausesChecks'))
# Room solve causes offline checks (check.cpp, features/room_solve_causes.cpp).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+(Join-Path $repo 'features')) /Feroom_solve_causes_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/room_solve_causes.cpp')
 if($LASTEXITCODE -ne 0){throw 'Room solve causes test compilation failed'}
 & '.\room_solve_causes_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Room solve causes checks failed'}
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+(Join-Path $repo 'features')) /Ferestart_settle_checks.exe (Join-Path $PSScriptRoot 'settle_check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Restart settle test compilation failed'}
 & '.\restart_settle_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Restart settle checks failed'}
} finally {Pop-Location}

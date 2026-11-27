param([string]$OutDir=(Join-Path $env:TEMP 'ApexEditInGameArgChecks'))
# Edit-in-game argument offline checks (check.cpp).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /Feedit_in_game_arg_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Edit-in-game argument test compilation failed'}
 & '.\edit_in_game_arg_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Edit-in-game argument checks failed'}
} finally {Pop-Location}

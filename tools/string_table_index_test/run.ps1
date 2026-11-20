param([string]$OutDir=(Join-Path $env:TEMP 'ApexStringTableIndexChecks'), [string]$Exe='')
# String table index offline checks (check.cpp). Optional: -Exe (TS3W.exe, read only) checks the five signatures.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if($Exe){$Exe=(Resolve-Path -LiteralPath $Exe).Path}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNOMINMAX /Festring_table_index_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'String table index test compilation failed'}
 $runArgs=@()
 if($Exe){$runArgs+=@('--exe',$Exe,'--sigs',(Join-Path $repo 'framework/game_addresses.cpp'))}
 & '.\string_table_index_checks.exe' @runArgs
 if($LASTEXITCODE -ne 0){throw 'String table index checks failed'}
} finally {Pop-Location}

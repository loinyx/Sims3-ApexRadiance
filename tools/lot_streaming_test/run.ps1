param([string]$OutDir=(Join-Path $env:TEMP 'ApexLotStreamingChecks'), [string]$CsvDir='', [string]$Exe='')
# Lot Streaming offline checks (check.cpp). Optional: -CsvDir (recorded LotLoadAnalysis CSVs) and -Exe (Steam TS3W.exe).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+(Join-Path $repo 'features')) /Felot_streaming_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Lot Streaming test compilation failed'}
 $runArgs=@()
 if($CsvDir){$runArgs+=@('--csv-dir',$CsvDir)}
 if($Exe){$runArgs+=@('--exe',$Exe)}
 & '.\lot_streaming_checks.exe' @runArgs
 if($LASTEXITCODE -ne 0){throw 'Lot Streaming checks failed'}
} finally {Pop-Location}

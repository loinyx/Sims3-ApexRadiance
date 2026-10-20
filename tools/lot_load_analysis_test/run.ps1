param([string]$OutDir=(Join-Path $env:TEMP 'ApexLotAnalysisChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) ('/I'+$repo) /Felot_analysis_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Lot analysis test compilation failed'}
 & '.\lot_analysis_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Lot analysis checks failed'}
 $csv=Get-ChildItem -LiteralPath $OutDir -Filter 'LotLoadAnalysis-*.csv' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
 $rows=@(Import-Csv -LiteralPath $csv.FullName)
 if($rows.Count -ne 4 -or @($rows[0].PSObject.Properties).Count -ne 19){throw 'CSV schema/count failed'}
 if($rows[0].kind -ne 'request' -or $rows[1].stage_before -ne $rows[1].stage_after -or $rows[-1].done -ne 'true'){throw 'CSV event sequence failed'}
 if([double]$rows[-1].request_to_done_ms -lt 0 -or [double]$rows[1].call_elapsed_ms -lt 1){throw 'CSV elapsed-time conversion failed'}
 if(@($rows | Where-Object {[double]$_.call_elapsed_ms -lt 0 -or [double]$_.call_elapsed_ms -gt 1000}).Count){throw 'CSV duration range failed'}
 Write-Host 'CSV request, wait and terminal event checks passed'
} finally {Pop-Location}

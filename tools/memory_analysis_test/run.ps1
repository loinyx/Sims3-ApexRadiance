param([string]$OutDir=(Join-Path $env:TEMP 'ApexMemoryAnalysisChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT /DPSAPI_VERSION=1 ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) ('/I'+$repo) /Fememory_analysis_checks.exe (Join-Path $PSScriptRoot 'check.cpp') /link /LARGEADDRESSAWARE psapi.lib
 if($LASTEXITCODE -ne 0){throw 'Memory analysis check compilation failed'}
 & '.\memory_analysis_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Memory analysis checks failed'}
 $csv=Get-ChildItem -LiteralPath $OutDir -Filter 'MemoryAnalysis-*.csv' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
 $samples=@(Import-Csv -LiteralPath $csv.FullName)
 if($samples.Count -ne 4 -or $samples[0].PSObject.Properties.Count -contains 0){throw 'CSV sample count failed'}
 if(@($samples[0].PSObject.Properties).Count -ne 30){throw 'CSV schema failed'}
 & (Join-Path $repo 'tools/development/analyze-memory.ps1') -Csv $csv.FullName
} finally {Pop-Location}

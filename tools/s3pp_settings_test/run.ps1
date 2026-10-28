param([string]$OutDir=(Join-Path $env:TEMP 'ApexS3PPSettingsChecks'))
# Sims3Performance.ini reading and the two-line change (s3pp_settings_test.cpp)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+(Join-Path $repo 'framework')) /Fes3pp_settings_checks.exe (Join-Path $PSScriptRoot 's3pp_settings_test.cpp')
 if($LASTEXITCODE -ne 0){throw 'Sims3Performance settings test compilation failed'}
 & '.\s3pp_settings_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Sims3Performance settings checks failed'}
} finally {Pop-Location}

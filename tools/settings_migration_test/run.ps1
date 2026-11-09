param([string]$OutDir=(Join-Path $env:TEMP 'ApexSettingsMigrationChecks'))
# The one-time move to the new lighting defaults (apex_lighting_defaults.h)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+$repo) ('/I'+(Join-Path $repo 'vcpkg_installed/x86-windows-static/include')) /Fesettings_migration_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Settings migration test compilation failed'}
 & '.\settings_migration_checks.exe' $repo
 if($LASTEXITCODE -ne 0){throw 'Settings migration checks failed'}
} finally {Pop-Location}

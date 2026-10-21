param([string]$OutDir=(Join-Path $env:TEMP 'ApexMemoryBudgetChecks'),[switch]$Standard)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 $flavor=if($Standard){@()}else{@('/DAPEX_NO_DEV_TOOLS','/DAPEX_MEMORY_TEST')}
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /Gy /MT /DNOMINMAX @flavor /DPSAPI_VERSION=1 ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) ('/I'+$repo) ('/I'+(Join-Path $repo 'vcpkg_installed/x86-windows-static/include')) /Fememory_budget_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/refpack_codec.cpp') (Join-Path $repo 'features/dxt_codec.cpp') /link /OPT:REF /LARGEADDRESSAWARE psapi.lib
 if($LASTEXITCODE -ne 0){throw 'Memory budget check compilation failed'}
 & '.\memory_budget_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Memory budget checks failed'}
} finally {Pop-Location}

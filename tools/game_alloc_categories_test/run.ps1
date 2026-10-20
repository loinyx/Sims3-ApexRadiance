param([string]$OutDir=(Join-Path $env:TEMP 'ApexGameAllocCategoryChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) ('/I'+$repo) ('/I'+(Join-Path $repo 'vcpkg_installed/x86-windows-static/include')) /Fegame_alloc_category_checks.exe (Join-Path $PSScriptRoot 'check.cpp') /link /LARGEADDRESSAWARE
 if($LASTEXITCODE -ne 0){throw 'Game allocation category check compilation failed'}
 & '.\game_alloc_category_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Game allocation category checks failed'}
} finally {Pop-Location}
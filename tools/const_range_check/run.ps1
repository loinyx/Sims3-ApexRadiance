param([string]$OutDir=(Join-Path $env:TEMP 'ApexConstRangeCheck'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 ('/I'+(Join-Path $repo 'framework')) '/Feconst_range_check.exe' (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE){throw 'const_range_check compilation failed'}
 & (Join-Path $OutDir 'const_range_check.exe')
 if($LASTEXITCODE){throw 'const_range_check failed'}
} finally {Pop-Location}

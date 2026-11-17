param([string]$OutDir=(Join-Path $env:TEMP 'ApexGiPixelFilterCheck'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /Fegi_pixel_filter_check.exe (Join-Path $PSScriptRoot 'check.cpp') /link d3d9.lib d3dcompiler.lib user32.lib
 if($LASTEXITCODE){throw 'compilation failed'}
 & (Join-Path $OutDir 'gi_pixel_filter_check.exe')
 if($LASTEXITCODE){throw 'check failed'}
} finally {Pop-Location}

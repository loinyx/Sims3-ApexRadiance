param([string]$OutDir=(Join-Path $env:TEMP 'ApexSpriteSmoothChecks'))
# Banding Fix smoothed sprite textures, offline check (read-only: prints to stdout; the compiler's outputs stay in $OutDir)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /fp:precise /arch:SSE2 ('/I'+(Join-Path $repo 'features')) '/Fesprite_smooth_check.exe' (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/dxt_codec.cpp')
 if($LASTEXITCODE){throw 'sprite_smooth_check compilation failed'}
 & (Join-Path $OutDir 'sprite_smooth_check.exe')
 if($LASTEXITCODE){throw 'sprite_smooth_check failed'}
} finally {Pop-Location}

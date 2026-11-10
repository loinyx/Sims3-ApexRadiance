param([string]$OutDir=(Join-Path $env:TEMP 'ApexFrameLimiterChecks'))
# The frame limiter's schedule and measurement (features/frame_limiter_math.h)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+(Join-Path $repo 'features')) /Feframe_limiter_checks.exe (Join-Path $PSScriptRoot 'frame_limiter_test.cpp')
 if($LASTEXITCODE -ne 0){throw 'Frame limiter test compilation failed'}
 & '.\frame_limiter_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Frame limiter checks failed'}
} finally {Pop-Location}

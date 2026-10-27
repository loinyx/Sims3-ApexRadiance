param([string]$OutDir=(Join-Path $env:TEMP 'ApexCrtMathChecks'))
# CRT math replacements against the originals (check.cpp)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /arch:SSE2 /fp:precise /MT ('/I'+(Join-Path $repo 'features')) /Fecrt_math_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'CRT math test compilation failed'}
 & '.\crt_math_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'CRT math checks failed'}
} finally {Pop-Location}

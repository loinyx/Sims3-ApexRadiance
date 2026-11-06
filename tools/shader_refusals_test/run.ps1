param([string]$OutDir=(Join-Path $env:TEMP 'ApexShaderRefusalChecks'))
# Shaders that used to be refused (check.cpp)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+(Join-Path $repo 'features')) /Feshader_refusal_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp')
 if($LASTEXITCODE -ne 0){throw 'Shader refusal test compilation failed'}
 & '.\shader_refusal_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Shader refusal checks failed'}
} finally {Pop-Location}

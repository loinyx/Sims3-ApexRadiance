param([string]$OutDir=(Join-Path $env:TEMP 'ApexS3SSTexturesChecks'))
# S3SS.toml reading and the texture change (s3ss_textures_test.cpp)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+(Join-Path $repo 'framework')) /Fes3ss_textures_checks.exe (Join-Path $PSScriptRoot 's3ss_textures_test.cpp')
 if($LASTEXITCODE -ne 0){throw 'S3SS texture settings test compilation failed'}
 & '.\s3ss_textures_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'S3SS texture settings checks failed'}
} finally {Pop-Location}

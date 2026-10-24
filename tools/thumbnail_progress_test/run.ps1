param([string]$OutDir=(Join-Path $env:TEMP 'ApexThumbnailProgressChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
if (-not $env:ApexVcpkgRoot) { throw 'ApexVcpkgRoot must identify the configured dependencies' }
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $env:ApexVcpkgRoot 'include')) /Fethumbnail_progress_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Thumbnail progress test compilation failed' }
    & '.\thumbnail_progress_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Thumbnail progress checks failed' }
} finally { Pop-Location }

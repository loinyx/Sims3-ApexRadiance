param([Parameter(Mandatory=$true)][string]$Vertex, [Parameter(Mandatory=$true)][string]$Base,
      [Parameter(Mandatory=$true)][string]$Overlay, [Parameter(Mandatory=$true)][string]$Precomp,
      [string]$OutDir=(Join-Path $env:TEMP 'ApexWorldTerrainLayoutChecks'),
      [string]$LowVertex='', [string]$LowBase='', [string]$LowOverlay='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) /Feworld_terrain_layout_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'World terrain layout compilation failed' }
    if ($LowVertex -or $LowBase -or $LowOverlay) {
        if (-not ($LowVertex -and $LowBase -and $LowOverlay)) { throw 'All low-layer fixtures are required together' }
        & '.\world_terrain_layout_checks.exe' $Vertex $Base $Overlay $Precomp $LowVertex $LowBase $LowOverlay
    } else {
        & '.\world_terrain_layout_checks.exe' $Vertex $Base $Overlay $Precomp
    }
    if ($LASTEXITCODE -ne 0) { throw 'World terrain layout checks failed' }
} finally { Pop-Location }

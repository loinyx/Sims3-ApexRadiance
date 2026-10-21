param([string]$Shader = '', [string]$Trunk = '', [string]$Precomp = '', [string]$OutDir = (Join-Path $env:TEMP 'ApexFoliageVertexChecks'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $Shader) {
    $Shader = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE 'Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures') -Recurse -Filter 'VS_9A10A3A8.bin' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Shader) { throw 'A captured foliage vertex shader (VS_9A10A3A8.bin) is needed: pass -Shader' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $repo) ('/I' + (Join-Path $repo 'features')) /Fefoliage_vertex_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/foliage_vertex_lamps.cpp') d3d9.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Foliage vertex check compilation failed' }
    # -Precomp: the game's Shaders_Win32.precomp (Game\Bin), for the trees' sun shadow over every pixel shader
    if ($Trunk -and $Precomp) { & '.\foliage_vertex_checks.exe' $Shader $Trunk $Precomp } elseif ($Trunk) { & '.\foliage_vertex_checks.exe' $Shader $Trunk } else { & '.\foliage_vertex_checks.exe' $Shader }
    if ($LASTEXITCODE -ne 0) { throw 'Foliage vertex checks failed' }
} finally { Pop-Location }

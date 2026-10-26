param([Parameter(Mandatory=$true)][string]$Precomp, [string]$D3d9 = '', [string]$OutDir = (Join-Path $env:TEMP 'ApexBandingFixCheck'))
# Banding Fix shader copies, offline check (read-only: prints to stdout; the compiler's outputs stay in $OutDir)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'banding_fix_check.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp') d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Banding Fix check did not compile' }
    $extra = if ($D3d9) { @('--d3d9', (Resolve-Path -LiteralPath $D3d9).Path) } else { @() }
    & $exe (Resolve-Path -LiteralPath $Precomp).Path @extra
    if ($LASTEXITCODE -ne 0) { throw 'Banding Fix check failed' }
} finally { Pop-Location }

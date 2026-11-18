param([string[]]$Worlds = @(), [int]$Lamps = 12, [switch]$Probe, [string]$OutDir = (Join-Path $env:TEMP 'ApexRtLampsTest'))
# Real-time outdoor lamps, offline (read-only: prints to stdout; the compiler's outputs stay in -OutDir). -Worlds: World.txt
# files of Full captures (their story grids, placed floors and outdoor lamps); -Lamps: lamps per lot.
# 2.12.3 + RT: part 2 of check.cpp (the room solve's grid walk of 2.12.4, with its penumbra and opening profiles, extracted
# from features/level_light_share.cpp) does not apply to this build's grid walk and is not compiled (no RT_GRID_EXTRACT);
# parts 1 and 3 (the shadows against the CPU reference, the bake split and the wall blur) run as on the source branch.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'rt_lamps_test.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /fp:precise /DNOMINMAX ('/I' + $repo) ('/I' + (Join-Path $repo 'framework')) ('/I' + $OutDir) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/rt_lamp_gpu.cpp') d3d9.lib d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Real-time outdoor lamps test did not compile' }
    $args2 = @('--lamps', "$Lamps") + $(if ($Probe) { @('--probe') } else { @() }) + @($Worlds | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
    & $exe @args2
    if ($LASTEXITCODE -ne 0) { throw 'Real-time outdoor lamps test failed' }
} finally { Pop-Location }

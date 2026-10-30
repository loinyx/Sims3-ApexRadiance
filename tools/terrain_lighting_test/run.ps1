param([Parameter(Mandatory=$true)][string]$Captures, [string]$OutDir = (Join-Path $env:TEMP 'ApexTerrainLightingTests'), [string]$ReferenceSource = '', [string]$EditReferenceSource = '', [string]$WorldReferenceSource = '', [string]$WallCapture = '', [string]$InstancedCapture = '', [string]$CompositionCapture = '', [string]$ObjectLawRoots = '')
$ErrorActionPreference = 'Stop'
# Object, fence and window lamp law coverage: every ps_3_0 under these ';'-separated folders (read-only)
$env:APEX_OBJECT_LAW_ROOTS = $ObjectLawRoots
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
$Captures = (Resolve-Path -LiteralPath $Captures).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'terrain_lighting_test.exe'
# The test prints to stdout only. The compiler's outputs stay in this test directory.
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Terrain checks did not compile' }
    if ($ReferenceSource) { & $exe $Captures (Join-Path $repo 'features/lot_light_bridge.cpp') $ReferenceSource $WallCapture $InstancedCapture $CompositionCapture }
    else { & $exe $Captures }
    if ($LASTEXITCODE -ne 0) { throw 'Terrain checks failed' }
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/Fe' + (Join-Path $OutDir 'world_lamp_test.exe')) (Join-Path $repo 'tools/world_lamp_test/check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'World lamp checks did not compile' }
    & (Join-Path $OutDir 'world_lamp_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'World lamp checks failed' }
    & (Join-Path $PSScriptRoot 'run_queue_checks.ps1') -Repo $repo -OutDir (Join-Path $OutDir 'queue')
    & (Join-Path $PSScriptRoot 'run_chunk_map_checks.ps1') -Repo $repo -OutDir (Join-Path $OutDir 'chunk-map')
    & (Join-Path $PSScriptRoot 'run_resource_checks.ps1') -Repo $repo -OutDir (Join-Path $OutDir 'resource')
    & (Join-Path $PSScriptRoot 'run_lamp_state_checks.ps1') -Repo $repo -OutDir (Join-Path $OutDir 'edit') -Baseline $EditReferenceSource
    & (Join-Path $PSScriptRoot 'run_dispatch_checks.ps1') -Repo $repo -OutDir (Join-Path $OutDir 'dispatch')
    & (Join-Path $PSScriptRoot 'run_world_pool_checks.ps1') -Repo $repo -OutDir (Join-Path $OutDir 'world') -Baseline $WorldReferenceSource
} finally { Pop-Location }

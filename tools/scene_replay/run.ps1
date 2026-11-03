param(
    [string]$OutDir = (Join-Path $env:TEMP 'ApexSceneReplay'),
    [string[]]$Scenes = @((Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Electronic Arts\The Sims 3\Apex Radiance\Captures')),
    [string]$Baseline = '',
    [switch]$UpdateBaseline,
    [switch]$SelfTest,
    [string]$Detail = ''
)
# Lighting scene replay (features/scene_record.h, docs/development/scene-replay.md): compiles replay.cpp against the production
# headers and runs it over the recorded scenes (every .apexscene under -Scenes). Read-only: writes the compiler outputs to
# -OutDir and, with -UpdateBaseline, the baseline file.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /fp:precise ('/I' + $repo) ('/I' + (Join-Path $repo 'features')) /Fescene_replay.exe (Join-Path $PSScriptRoot 'replay.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Scene replay compilation failed' }
    $runArgs = @()
    if ($SelfTest) { $runArgs += '--selftest' }
    if ($Baseline) { $runArgs += @('--baseline', $Baseline) }
    if ($UpdateBaseline) { $runArgs += '--update-baseline' }
    if ($Detail) { $runArgs += @('--detail', $Detail) }
    & './scene_replay.exe' @runArgs @Scenes
    if ($LASTEXITCODE -ne 0) { throw 'Scene replay found a regression or a failed self-test' }
} finally { Pop-Location }

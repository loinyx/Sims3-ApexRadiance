param([ValidateSet('Smoke','Full','Lighting')][string]$Suite = 'Smoke')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
$out = Join-Path $repo 'outputs/checks'
New-Item -ItemType Directory -Path $out -Force | Out-Null
if ($Suite -eq 'Lighting') {
    # Pure lighting harnesses: no captures, no shader package. Each runner compiles its harness and throws on a failure.
    foreach ($name in @('stairwell_bounce_test', 'opening_bounce_test', 'arch_ambient_test', 'lighting_prepare_test', 'render_cost_test')) {
        & (Join-Path $repo "tools/$name/run.ps1") -OutDir (Join-Path $out $name) | Tee-Object -FilePath (Join-Path $out ($name + '.txt'))
    }
    Write-Output 'CPU Lighting suite passed. Gameplay validation remains separate.'
    return
}
$cases = @(
    @{ Name='dxt'; Source='tools/dxt_test/dxt_test.cpp'; Implementation='features/dxt_codec.cpp'; Args=@('--blocks','100000') },
    @{ Name='refpack'; Source='tools/refpack_test/refpack_test.cpp'; Implementation='features/refpack_codec.cpp'; Args=@('--quick') },
    @{ Name='cas-sort'; Source='tools/cas_sort_test/cas_sort_test.cpp'; Implementation='features/cas_tri_sort.cpp'; Args=@('--quick') }
)
Push-Location $out
try {
    foreach ($case in $cases) {
        $exe = Join-Path $out ($case.Name + '.exe')
        & cl.exe /nologo /O2 /EHsc /std:c++20 /arch:SSE2 /fp:precise /MT /utf-8 ('/I'+(Join-Path $repo 'features')) ('/Fe:'+ $exe) (Join-Path $repo $case.Source) (Join-Path $repo $case.Implementation)
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($case.Name)" }
        $caseArgs = if ($Suite -eq 'Full') { @() } else { $case.Args }
        & $exe @caseArgs 2>&1 | Tee-Object -FilePath (Join-Path $out ($case.Name + '.txt'))
        if ($LASTEXITCODE -ne 0) { throw "Checks failed: $($case.Name)" }
    }
} finally { Pop-Location }
# Banding Fix glow sprites: the shader family, DXT decoding and the smoothing of a game-encoded glow
& (Join-Path $repo 'tools/sprite_smooth_test/run.ps1') -OutDir (Join-Path $out 'sprite_smooth') | Tee-Object -FilePath (Join-Path $out 'sprite_smooth.txt')
# High-precision scene: pure logic, and its resolve shader on a native D3D9 device when one exists (SKIP otherwise)
& (Join-Path $repo 'tools/scene_target_test/run.ps1') -OutDir (Join-Path $out 'scene_target') | Tee-Object -FilePath (Join-Path $out 'scene_target.txt')
Write-Output "CPU $Suite suite passed. Gameplay and D3D9/DXVK validation remain separate."

param([ValidateSet('Smoke','Full')][string]$Suite = 'Smoke')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
$out = Join-Path $repo 'outputs/checks'
New-Item -ItemType Directory -Path $out -Force | Out-Null
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
    # ReShade FX: the vendored compiler builds as C++17 (its u8string use predates C++20), the rest as C++20; every
    # repository test effect must compile to shader model 3 with D3DCompile
    $fxObj = Join-Path $out 'reshadefx'
    New-Item -ItemType Directory -Path $fxObj -Force | Out-Null
    $fxSources = Get-ChildItem (Join-Path $repo 'third_party/reshadefx') -Filter *.cpp | ForEach-Object FullName
    & cl.exe /nologo /c /O1 /EHsc /std:c++17 /MT /utf-8 /W3 ('/Fo'+$fxObj+'\') @fxSources
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed: reshadefx' }
    $fxExe = Join-Path $out 'fx-check.exe'
    & cl.exe /nologo /O1 /EHsc /std:c++20 /MT /utf-8 ('/I'+$repo) ('/I'+(Join-Path $repo 'third_party/reshadefx')) ('/Fe:'+$fxExe) ('/Fo'+$out+'\') (Join-Path $repo 'tools/reshade_fx_test/fx_check.cpp') (Join-Path $repo 'features/reshade_fx_compile.cpp') (Get-ChildItem $fxObj -Filter *.obj | ForEach-Object FullName)
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed: fx-check' }
    & $fxExe --expect-ok (Join-Path $repo 'tools/reshade_fx_test/effects') 2>&1 | Tee-Object -FilePath (Join-Path $out 'fx-check.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Checks failed: fx-check' }
} finally { Pop-Location }
Write-Output "CPU $Suite suite passed. Gameplay and D3D9/DXVK validation remain separate."

param(
    [Parameter(Mandatory = $true)][string]$BridgeSrc, # a bridge source tree built by native/remix-bridge/build.cmd (its src\ folder)
    [string]$Scene = (Join-Path $env:TEMP 'ApexGiServerTest/gi_scene.bin'), # tools/gi_server_test/run.ps1 -TestArgs --dump <file>
    [string[]]$Modes = @('on', 'compute'),
    [string]$OutDir = (Join-Path $env:TEMP 'ApexGiComposeTest'))
# Real-time GI compose (S2-01b) through the built bridge pair (check.cpp). The test app is a 32-bit Direct3D 9 app run by
# the bridge's own launcher (bridge\test\rtx\unit\test_remix_api_bridge_app.py), which deploys it next to the client and
# the server in -OutDir and runs it with bridge timeouts off and on. Read-only apart from -OutDir. Needs Python and a
# desktop session with a GPU.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not (Test-Path -LiteralPath $Scene)) { throw "No scene dump at ${Scene}: run tools/gi_server_test/run.ps1 -TestArgs '--dump','$Scene' first" }
$bridge = Join-Path $BridgeSrc 'bridge'
if (-not (Test-Path -LiteralPath (Join-Path $bridge '_output/.trex/NvRemixBridge.exe'))) { throw "No built bridge pair in $bridge\_output" }
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler # (x86)
$bin = Join-Path $OutDir 'bin'
New-Item -ItemType Directory -Path $bin -Force | Out-Null
$exe = Join-Path $bin 'gi_compose_test.exe'
Push-Location $bin
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /DNOMINMAX ('/I' + $repo) ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/gi_compose.cpp') d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'check.cpp did not compile' }
} finally { Pop-Location }
$env:REMIX_GI_TEST_SCENE = (Resolve-Path -LiteralPath $Scene).Path
foreach ($mode in $Modes) {
    $env:REMIX_GI_TEST_CASE = $mode
    & python (Join-Path $bridge 'test/rtx/unit/test_remix_api_bridge_app.py') --testExe gi_compose_test.exe --executablesDir $bin --bridgeDir (Join-Path $bridge '_output') --artifactDir (Join-Path $OutDir "run_$mode") --timeout 300
    if ($LASTEXITCODE -ne 0) { throw "gi_compose_test failed in mode $mode" }
}
Write-Output 'GI compose tests passed'

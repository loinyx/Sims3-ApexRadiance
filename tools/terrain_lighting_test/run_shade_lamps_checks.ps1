param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexShadeLampTests'))
# The production ShadeLamps, MovedApart and RefusalHolds (features/lot_light_bridge.cpp) and LampMoving (features/drag_lite.cpp) with a
# fake lamp list, fake lights and a fake clock (shade_lamps_fixture.cpp). Read-only: writes only the generated source and the
# compiler outputs to -OutDir.
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
function Read-Function([string]$text, [string]$signature) {
    $start = $text.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing function: $signature" }
    $tail = $text.Substring($start)
    $end = [regex]::Match($tail, '(?m)^}\r?$')
    if (-not $end.Success) { throw "Missing function end: $signature" }
    return $tail.Substring(0, $end.Index + $end.Length)
}
function Read-Line([string]$text, [string]$pattern) {
    $m = [regex]::Match($text, '(?m)^' + $pattern + '.*$')
    if (-not $m.Success) { throw "Missing line: $pattern" }
    return $m.Value
}
$bridge = Get-Content -LiteralPath (Join-Path $Repo 'features/lot_light_bridge.cpp') -Raw
$drag = Get-Content -LiteralPath (Join-Path $Repo 'features/drag_lite.cpp') -Raw
$production = @(
    (Read-Line $bridge 'constexpr float kMoveTol = '),
    (Read-Line $bridge 'constexpr size_t kMaxOtherLamps = '),
    (Read-Line $bridge 'constexpr float kShadeMargin = '),
    (Read-Line $bridge 'DWORD g_classicHeldAt = '),
    (Read-Function $bridge 'bool RefusalHolds(uint64_t& refusedKey, uint64_t occ) {'),
    (Read-Function $bridge 'bool MovedApart(const float* a, const float* b) {'),
    (Read-Function $bridge 'std::vector<GroundShadow::Lamp> ShadeLamps(')
) -join "`n"
$dragFunctions = (Read-Function $drag 'bool Within(const std::atomic<DWORD>& at, unsigned long ms) {') + "`n" + (Read-Function $drag 'bool LampMoving() {')
$fixture = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'shade_lamps_fixture.cpp') -Raw).Replace('/* PRODUCTION_FUNCTIONS */', $production).Replace('/* DRAG_FUNCTIONS */', $dragFunctions)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'shade_lamps.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $Repo) ('/I' + (Join-Path $Repo 'features')) /Fe:shade_lamp_checks.exe shade_lamps.generated.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Shade lamp checks failed to compile' }
    & './shade_lamp_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Shade lamp checks failed' }
} finally { Pop-Location }

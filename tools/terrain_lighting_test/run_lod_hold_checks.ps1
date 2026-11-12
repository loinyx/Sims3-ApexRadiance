param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexLodHoldTests'))
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$bridge = Get-Content -LiteralPath (Join-Path $Repo 'features/lot_light_bridge.cpp') -Raw
$patch = Get-Content -LiteralPath (Join-Path $Repo 'patches/night_terrain_relight_patch.cpp') -Raw
function Read-Function([string]$text, [string]$signature) {
    $start = $text.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing function: $signature" }
    $tail = $text.Substring($start)
    $firstEnd = $tail.IndexOf("`n", [StringComparison]::Ordinal)
    if ($firstEnd -lt 0) { $firstEnd = $tail.Length }
    $first = $tail.Substring(0, $firstEnd).TrimEnd("`r")
    if ($first.EndsWith('}')) { return $first }
    $end = [regex]::Match($tail, '(?m)^}\r?$')
    if (-not $end.Success) { throw "Missing function end: $signature" }
    return $tail.Substring(0, $end.Index + $end.Length)
}
function Read-Struct([string]$text, [string]$signature) {
    $start = $text.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing struct: $signature" }
    $tail = $text.Substring($start)
    $end = [regex]::Match($tail, '(?m)^};\r?$')
    if (-not $end.Success) { throw "Missing struct end: $signature" }
    return $tail.Substring(0, $end.Index + $end.Length)
}
$structs = Read-Struct $bridge 'struct LotLampState {'
$helpers = @('bool IsPlainType(', 'float LampLight(', 'bool InBake(', 'bool MovedApart(const float* a, const float* b) {', 'bool LightDiffers(', 'bool LightChanged(',
    'bool ValuesChanged(', 'bool RawChanged(', 'bool LodHolds(') | ForEach-Object { Read-Function $bridge $_ }
$varsStart = $bridge.IndexOf('constexpr auto kDetailSwitch', [StringComparison]::Ordinal)
$varsEnd = $bridge.IndexOf('uintptr_t LotRenderers()', [Math]::Max($varsStart, 0), [StringComparison]::Ordinal)
if ($varsStart -lt 0 -or $varsEnd -lt 0) { throw 'Missing the detail set state' }
$detail = @($bridge.Substring($varsStart, $varsEnd - $varsStart)) + (@('void ReadDetailSwitches(', 'int DetailOf(') | ForEach-Object { Read-Function $bridge $_ })
$diff = @('static std::pair<std::vector<BakeLamp>::const_iterator', 'bool BakeTakes(', 'BakeDiff DiffBake(') | ForEach-Object { Read-Function $bridge $_ }
$bakeTest = @('bool OriginalWorldLightTest(', 'bool IsOutdoorLotLamp(', 'int BakeTest(') | ForEach-Object { Read-Function $patch $_ }
# The lot table: LotRect and g_lotRects, then g_lotRectsGen, g_lotBase and RefreshLotRects (to its closing brace), and LotGroundY
$rectStart = $bridge.IndexOf('struct LotRect {', [StringComparison]::Ordinal)
$rectEnd = $bridge.IndexOf('std::atomic<bool> g_softEdges', [Math]::Max($rectStart, 0), [StringComparison]::Ordinal)
if ($rectStart -lt 0 -or $rectEnd -lt 0) { throw 'Missing the lot table' }
$lotTable = @($bridge.Substring($rectStart, $rectEnd - $rectStart), (Read-Function $bridge 'uint32_t g_lotRectsGen = 1;'))
$ground = Read-Function $bridge 'bool LotGroundY('
# The fixture restates the read step and relies on the tracking's unchanged-lamp test: both must still be production's.
foreach ($line in @('LodHolds(p->second, s, DetailOf(s.lot, now))', 's.held = true, s.heldFade = p->second.held ? p->second.heldFade : p->second.fade;',
        's.baked = s.held || InBake(s);', 'if (!RawChanged(p, s) && p.baked == s.baked)', '(s.flags & 0x61) == 0x21) { // a disabled lot lamp')) {
    if ($bridge.IndexOf($line, [StringComparison]::Ordinal) -lt 0) { throw "The LOD hold read step changed; update lod_hold_fixture.cpp: $line" }
}
$fixture = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lod_hold_fixture.cpp') -Raw
$fixture = $fixture.Replace('/* PRODUCTION_STRUCTS */', $structs)
$fixture = $fixture.Replace('/* PRODUCTION_HELPERS */', ($helpers -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_DETAIL */', ($detail -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_DIFF */', ($diff -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_BAKE_TEST */', ($bakeTest -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_LOT_TABLE */', ($lotTable -join "`n`n"))
$fixture = $fixture.Replace('/* PRODUCTION_GROUND */', $ground)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'lod_hold.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT "/I$Repo" /Fe:lod_hold_checks.exe lod_hold.generated.cpp
    if ($LASTEXITCODE -ne 0) { throw 'LOD hold checks failed to compile' }
    & './lod_hold_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'LOD hold checks failed' }
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Repo 'features/lot_light_bridge.cpp'), (Join-Path $Repo 'patches/night_terrain_relight_patch.cpp')
} finally { Pop-Location }

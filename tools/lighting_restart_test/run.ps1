param([string]$OutDir=(Join-Path $env:TEMP 'ApexLightingRestartChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
function Slice([string]$text,[string]$from,[string]$to) {
 $start=$text.IndexOf($from)
 if($start -lt 0){throw "Missing production block: $from"}
 $end=$text.IndexOf($to,$start)
 if($end -lt 0){throw "Missing production block end: $to"}
 $text.Substring($start,$end-$start)
}
$level=Get-Content -Raw -LiteralPath (Join-Path $repo 'features/level_light_share.cpp')
$gpu=Get-Content -Raw -LiteralPath (Join-Path $repo 'features/rt_outdoor_lamps.cpp')
$patch=Get-Content -Raw -LiteralPath (Join-Path $repo 'patches/night_terrain_relight_patch.cpp')
$parts=@((Slice $level 'struct RestartStory {' 'void RefreshCompletedLotRigs()'),(Slice $level 'bool BeginLightingRestart() {' 'void SetFoundationWalls('))
Set-Content -LiteralPath (Join-Path $OutDir 'restart_extracted.h') -Value ($parts -join "`n") -Encoding UTF8
Set-Content -LiteralPath (Join-Path $OutDir 'proxy_extracted.h') -Value (Slice $gpu 'void BuildProxy(uintptr_t tracker, const void* room0) {' "`n} // namespace") -Encoding UTF8
Set-Content -LiteralPath (Join-Path $OutDir 'coordinator_extracted.h') -Value (Slice $patch 'void PumpLightingRestart() {' '// ---- menu helpers ----') -Encoding UTF8
$fallback=@((Slice $gpu 'struct Ask {' 'std::vector<std::unique_ptr<Slot>> g_slots;'),(Slice $gpu 'bool RestartSolveTerminal(' 'std::unordered_map<const void*, Ask> g_askedSolve;'),(Slice $gpu 'bool RestartIncomplete() {' '// ---- the light tree thread ----'))
Set-Content -LiteralPath (Join-Path $OutDir 'fallback_extracted.h') -Value ($fallback -join "`n") -Encoding UTF8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+$repo) ('/I'+$OutDir) /Felighting_restart_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Lighting restart checks compilation failed'}
 & './lighting_restart_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Lighting restart checks failed'}
} finally {Pop-Location}

param([string]$OutDir=(Join-Path $env:TEMP 'ApexLightingPrepareChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
$lot=Get-Content -LiteralPath (Join-Path $repo 'features/lot_light_bridge.cpp') -Raw
$share=Get-Content -LiteralPath (Join-Path $repo 'features/level_light_share.cpp') -Raw
function Block([string]$source,[string]$name) {
    $m=[regex]::Match($source,'(?ms)^'+[regex]::Escape($name)+'\([^\n]*\) \{.*?^\}')
    if(-not $m.Success) { throw ('Production block missing: '+$name) }
    return $m.Value
}
$blocks=@((Block $lot 'uint64_t GapKey'), ((Block $lot 'void FenceGapsBuild').Replace('std::hypot','CountedHypot')))
$start=$share.IndexOf('struct StoryMapSlot {')
$end=$share.IndexOf('// The drawn ambient',$start)
if($start -lt 0 -or $end -lt 0) { throw 'Map slot block missing' }
$blocks += $share.Substring($start,$end-$start)
$blocks += 'inline uint32_t Avg4(uint32_t a, uint32_t b) { return (a | b) - (((a ^ b) >> 1) & 0x7F7F7F7Fu); }'
foreach($name in @('void BlurRow','void BlurColumn','bool CopyRows')) { $blocks += Block $share $name }
$start=$share.IndexOf('    long edges = 0, crossed = 0;', $share.IndexOf('void BlurWalls('))
$end=$share.IndexOf("`n}",$start)
if($start -lt 0 -or $end -lt 0) { throw 'Wall blur loop missing' }
$body=$share.Substring($start,$end-$start)
$signature='(uintptr_t atlas, uint32_t pitch, const std::vector<std::vector<uint32_t>>& before, const std::vector<GhostRows>& ghosts, std::vector<uint32_t>& ext, uint32_t passes) { const int jobs = static_cast<int>(before.size());'
$blocks += 'void PreparedBlur'+$signature+"`n"+$body+"`n}"
$old=$body.Replace('ext.resize(static_cast<size_t>(total) * n);','ext.assign(static_cast<size_t>(total) * n, 0u);')
if($old -eq $body) { throw 'Wall blur resize missing' }
$blocks += 'void ReferenceBlur'+$signature+"`n"+$old+"`n}"
[IO.File]::WriteAllText((Join-Path $OutDir 'production.h'), ($blocks -join "`n"), [Text.UTF8Encoding]::new($false))
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /fp:precise /MT ('/I'+$OutDir) /Felighting_prepare_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Lighting preparation test compilation failed' }
    & '.\lighting_prepare_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Lighting preparation checks failed' }
} finally { Pop-Location }

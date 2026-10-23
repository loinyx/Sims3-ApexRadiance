param([string]$OutDir=(Join-Path $env:TEMP 'ApexRoofedOpeningsChecks'))
# Outdoor light through openings (level_light_share.cpp): extracts the story grid, the openings its cuts give it (doors
# closed for lamps or not, glass doors open), its ray walk, the roofed rooms' doors and
# the outside walls' copy with its patches (doors, windows, arches, half walls) from production and runs check.cpp on
# synthetic grids, with the ground shade (ground_lamp_shadow.h) and the patch test (wall_openings.h). Read-only: writes only the extracted header and the compiler
# outputs to -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$source=Get-Content -LiteralPath (Join-Path $repo 'features/level_light_share.cpp') -Raw
function Between([string]$from,[string]$to){
 $s=$source.IndexOf($from)
 if($s -lt 0){throw ('Production block missing: '+$from)}
 $e=$source.IndexOf($to,$s)
 if($e -lt $s){throw ('Production block end missing: '+$to)}
 return $source.Substring($s,$e-$s)
}
$parts=@(
 (Between 'struct CutRec {' 'struct StoryCuts {'),
 (Between 'constexpr float kGridSoft = 0.04f;' 'struct GridTileRead {'),
 (Between 'uint32_t EdgeKey(int x0' '// At 0x00B0D860 (Add)'),
 (Between 'void OpeningDefaults(' '// Opening shapes from the cutout masks'),
 (Between 'struct GridWhy {' '// A lamp to a point (world) of a roofless room'),
 (Between 'struct YardOutline {' 'void OutlineOf('),
 (Between 'struct YardBox {' 'std::unordered_map<uint64_t, YardBox> g_yardBoxes;'),
 (Between 'constexpr int kRoofedMaxLamps' 'std::atomic<bool> g_roofedOpenOn'),
 (Between 'struct RoofedDoor {' '// The roofed rooms with doors at their last gather'),
 (Between 'struct WallSeg {' 'constexpr DWORD kPatchRetryMs'),
 (Between 'bool SameGeometry(' "// The story's grid for its walls' patches"),
 (Between 'float SegPass(' '// The share of the lamp')
)
Set-Content -LiteralPath (Join-Path $OutDir 'extracted_roofed.h') -Value ($parts -join "`n") -Encoding UTF8
$wallParts = @(
 (Between 'struct WallBlock {' '// After FUN_006ac070'),
 (Between 'struct WallCellH {' 'struct WallHeightRec {'),
 (Between 'bool WallLightBase(' 'bool LowFoot('),
 (Between 'bool RecutCellV(' 'void __fastcall WallUvHook('),
 (Between 'long MoveWallFootSamples(' 'bool MoveWallFoot(')
)
Set-Content -LiteralPath (Join-Path $OutDir 'extracted_wall_span.h') -Value ($wallParts -join "`n") -Encoding UTF8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) ('/I'+$OutDir) /Feroofed_openings_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Roofed openings checks compilation failed'}
 & './roofed_openings_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Roofed openings checks failed'}
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+$repo) ('/I'+$OutDir) /Fewall_native_checks.exe (Join-Path $PSScriptRoot 'wall_native_check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Native wall checks compilation failed'}
 & './wall_native_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Native wall checks failed'}
}finally{Pop-Location}

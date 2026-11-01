# Replays Apex's own light tests of a roofed room's solve (the floor cut, InnerShadow, the outdoor lamps through openings,
# IndoorShadow, the soft light cut) on a RoomLampProbe dump, with the production functions extracted from
# features/level_light_share.cpp, and compares them with what the game test recorded. Read-only: writes only the extracted
# header and the compiler outputs to a fresh folder under %TEMP% (removed afterwards).
param([Parameter(Mandatory = $true)][string]$Dump)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;$env:PATH"
$source = Get-Content -LiteralPath (Join-Path $repo 'features/level_light_share.cpp') -Raw
function Between([string]$from, [string]$to) {
    $s = $source.IndexOf($from)
    if ($s -lt 0) { throw ('Production block missing: ' + $from) }
    $e = $source.IndexOf($to, $s)
    if ($e -lt $s) { throw ('Production block end missing: ' + $to) }
    return $source.Substring($s, $e - $s)
}
$parts = @(
    (Between 'struct Xform {' '// The same lamp must use the same fade'),
    (Between 'uintptr_t LightTile(uintptr_t mgr' '// Openings of story B'),
    (Between 'struct CutRec {' '// A cut post on a story'),
    (Between 'constexpr float kGridSoft = 0.04f;' 'struct GridTileRead {'),
    (Between 'struct GridWhy {' '// A lamp to a point (world) of a roofless room'),
    (Between 'float YardPassImpl(' 'std::atomic<long> g_gridFaults{0};'),
    (Between 'struct Cross {' 'std::unordered_map<uintptr_t, RoomInfo> g_rooms;'),
    (Between 'struct SolveCtx {' '// ---- One daytime curve'),
    (Between 'const Cross* FindCross(const RoomInfo& info, uintptr_t light) {' 'int HomeFloor('),
    (Between 'struct PassDebug {' '// The F8 text of a PassWhy'),
    (Between 'float FadeAtThreshold(' "// The game's point solve with the soft light cut"),
    (Between 'struct RayEntry {' '// Where an outdoor lamp of a lower story enters'),
    (Between 'struct SlabRay {' 'bool OutdoorEntry(const RoomInfo& info, int home'),
    (Between 'std::atomic<long> g_roofedBelowTests' '// Indoor light through openings (ShareInnerLamps)'),
    (Between 'float InnerDownGateImpl(' 'float InnerDownGate(const RoomInfo& info'),
    (Between 'float InnerPass(const RoomInfo& info, void* light, const float* pointW, float* lampW, GridWhy& why) {' '// The walls of the stories between a lamp'),
    (Between '// 4.: a lamp of another story reaches the point only through' 'float IndoorPass(const RoomInfo& info, const Cross& c'),
    (Between 'float RoomNormOf(const BYTE* room) {' 'float InnerNormScaleAt('),
    (Between 'std::atomic<float> g_openGround{1.0f}, g_openWall{1.0f};' '// The outdoor sum at the opening'),
    (Between 'bool OwnFloorBlocksAt(' 'bool OwnFloorBlocks(const unsigned char* room, void* light')
)
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
# a fresh folder and exe name per run: a just-built exe can stay locked by the virus scanner
$out = Join-Path $env:TEMP ("apex_paths_replay_$PID")
New-Item -ItemType Directory -Force $out | Out-Null
try {
    Set-Content -LiteralPath (Join-Path $out 'extracted_apex_paths.h') -Value ($parts -join "`n") -Encoding UTF8
    $exe = Join-Path $out "apex_paths_replay_$PID.exe"
    $log = & cl.exe /nologo /O2 /EHsc /std:c++20 /MT ('/I' + $repo) ('/I' + $out) (Join-Path $PSScriptRoot 'replay.cpp') "/Fe$exe" "/Fo$out\\"
    if ($LASTEXITCODE -ne 0) { $log; throw 'compile failed' }
    & $exe $Dump
    if ($LASTEXITCODE -ne 0) { throw 'replay failed' }
} finally {
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
}

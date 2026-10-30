param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexTerrainChunkMapTests'))
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$source = Get-Content -LiteralPath (Join-Path $Repo 'features/lot_light_bridge.cpp') -Raw
function Read-Block([string]$signature, [string]$end) {
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing production block: $signature" }
    $tail = $source.Substring($start)
    $ending = [regex]::Match($tail, $end)
    if (-not $ending.Success) { throw "Missing end of production block: $signature" }
    return $tail.Substring(0, $ending.Index + $ending.Length)
}
$code = @(
    (Read-Block 'struct ChunkTex {' '(?m)^};\r?$'),
    (Read-Block 'std::map<std::pair<int, int>, ChunkTex> g_chunks;' '(?m)$'),
    (Read-Block 'std::unordered_map<IDirect3DBaseTexture9*, std::pair<int, int>> g_chunkOfTexture;' '(?m)$'),
    (Read-Block 'DWORD RecordWorldChunk(' '(?m)^}\r?$')
) -join "`n"
$fixture = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'chunk_map_fixture.cpp') -Raw).Replace('/* PRODUCTION_CODE */', $code)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'chunk_map_checks.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $Repo) /Fe:terrain_chunk_map_checks.exe chunk_map_checks.generated.cpp (Join-Path $Repo 'features/shader_patches.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Chunk map checks failed to compile' }
    & './terrain_chunk_map_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Chunk map checks failed' }
} finally { Pop-Location }

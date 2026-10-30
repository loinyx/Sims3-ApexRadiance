param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexEditDispatchTests'), [string]$Baseline = '')
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$source = Get-Content -LiteralPath (Join-Path $Repo 'patches/night_terrain_relight_patch.cpp') -Raw
$start = $source.IndexOf('void DecideEdit(', [StringComparison]::Ordinal)
$end = $source.IndexOf('    std::string diffText =', $start, [StringComparison]::Ordinal)
if ($start -lt 0 -or $end -le $start) { throw 'Production dispatcher guards changed; update coverage' }
$guards = $source.Substring($start, $end - $start) + "    dispatched++;`n}"
$fixture = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'edit_dispatch_fixture.cpp') -Raw).Replace('/* PRODUCTION_DISPATCH */', $guards)
$oldGuards = ''
if ($Baseline) {
    $oldSource = Get-Content -LiteralPath $Baseline -Raw
    $oldStart = $oldSource.IndexOf('void DecideEdit(', [StringComparison]::Ordinal)
    $oldEnd = $oldSource.IndexOf('    std::string diffText =', $oldStart, [StringComparison]::Ordinal)
    if ($oldStart -lt 0 -or $oldEnd -le $oldStart) { throw 'Missing baseline dispatcher' }
    $oldGuards = $oldSource.Substring($oldStart, $oldEnd - $oldStart).Replace('void DecideEdit(', 'void OldDecideEdit(') + "    dispatched++;`n}"
    $fixture = "#define HAVE_BASELINE 1`nint g_sweepId = 0;`n" + $fixture
}
$fixture = $fixture.Replace('/* BASELINE_DISPATCH */', $oldGuards)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'dispatch.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $Repo) /Fe:edit_decide_checks.exe dispatch.generated.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Dispatch checks failed to compile' }
    & './edit_decide_checks.exe' # (no "patch" in the name: Windows asks to elevate such programs)
    if ($LASTEXITCODE -ne 0) { throw 'Dispatch checks failed' }
} finally { Pop-Location }

param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexStampedTests'))
# The production AllStamped (patches/night_terrain_relight_patch.cpp) with fake lights (stamped_fixture.cpp). Read-only:
# writes only the generated source and the compiler outputs to -OutDir.
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$source = Get-Content -LiteralPath (Join-Path $Repo 'patches/night_terrain_relight_patch.cpp') -Raw
$start = $source.IndexOf('constexpr float kStampedNear', [StringComparison]::Ordinal)
if ($start -lt 0) { throw 'Missing kStampedNear' }
$end = [regex]::Match($source.Substring($start), '(?m)^}\r?$')
if (-not $end.Success) { throw 'Missing the end of AllStamped' }
$function = $source.Substring($start, $end.Index + $end.Length)
if ($function.IndexOf('bool AllStamped(', [StringComparison]::Ordinal) -lt 0) { throw 'AllStamped no longer follows kStampedNear; update the extraction' }
$fixture = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'stamped_fixture.cpp') -Raw).Replace('/* PRODUCTION_FUNCTION */', $function)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $OutDir 'stamped.generated.cpp'), $fixture)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I' + $Repo) /Fe:stamped_checks.exe stamped.generated.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Stamped checks failed to compile' }
    & './stamped_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Stamped checks failed' }
} finally { Pop-Location }

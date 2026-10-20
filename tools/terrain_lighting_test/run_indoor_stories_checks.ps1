param([string]$OutDir = (Join-Path $env:TEMP 'ApexIndoorStoriesTests'), [string]$ReferenceSource = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Get-Content -LiteralPath (Join-Path $repo 'features/level_light_share.cpp') -Raw
$start = $source.IndexOf('float IndoorBoundaryPass(')
$end = $source.IndexOf('float IndoorPass(', $start)
if ($start -lt 0 -or $end -lt $start) { throw 'Production indoor pass not found' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$extracted = $source.Substring($start, $end-$start)
$basisStart = $source.IndexOf('void __fastcall BasisLightHook(')
$basisEnd = $source.IndexOf('// The share of a lamp of another story', $basisStart)
if ($basisStart -lt 0 -or $basisEnd -lt $basisStart) { throw 'Production basis light hook not found' }
$extracted += $source.Substring($basisStart, $basisEnd-$basisStart)
$defines = @()
if ($ReferenceSource) {
    $reference = Get-Content -LiteralPath $ReferenceSource -Raw
    $referenceStart = $reference.IndexOf('float IndoorBoundaryPass(')
    $referenceEnd = $reference.IndexOf('float IndoorPass(', $referenceStart)
    if ($referenceStart -lt 0 -or $referenceEnd -lt $referenceStart) { throw 'Reference indoor pass not found' }
    $extracted += $reference.Substring($referenceStart, $referenceEnd-$referenceStart).Replace('IndoorPassImpl(', 'ReferenceIndoorPass(').Replace('IndoorBoundaryPass(', 'ReferenceIndoorBoundaryPass(')
    $defines = @('/DAPEX_INDOOR_REFERENCE')
}
Set-Content -LiteralPath (Join-Path $OutDir 'extracted_indoor_stories.h') -Value $extracted -Encoding UTF8
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT @defines ('/I' + $OutDir) /Feindoor_stories_test.exe (Join-Path $PSScriptRoot 'indoor_stories_fixture.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Indoor stories checks did not compile' }
    & (Join-Path $OutDir 'indoor_stories_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Indoor stories checks failed' }
} finally { Pop-Location }

param([string]$OutDir=(Join-Path $env:TEMP 'ApexTransitionWatchChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT /DAPEX_NO_DEV_TOOLS /Fetransition_watch_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'framework/transition_watch.cpp') (Join-Path $repo 'framework/transition_calls.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Transition watch test compilation failed' }
    & '.\transition_watch_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Transition watch checks failed' }
    $deps=(Select-Xml -Path (Join-Path $repo 'ApexRadiance.vcxproj') -XPath '//*[local-name()="ApexVcpkgRoot"]' | Select-Object -First 1).Node.InnerText
    if (-not $deps -or $deps.Contains('$')) { $deps=$env:ApexVcpkgRoot }
    if (-not $deps) { throw 'ApexVcpkgRoot must identify the configured dependency root for native wrapper tests' }
    & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) ('/I'+(Join-Path $deps 'include')) /Fetransition_call_checks.exe (Join-Path $PSScriptRoot 'calls.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Transition native wrapper test compilation failed' }
    & '.\transition_call_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Transition native wrapper checks failed' }
} finally { Pop-Location }

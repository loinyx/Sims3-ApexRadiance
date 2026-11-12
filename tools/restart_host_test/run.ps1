param([string]$OutDir=(Join-Path $env:TEMP 'ApexRestartHostChecks'))
# The restart host's decision, relaunch command line and save pick (native/ts3wrap/restart_channel.h)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /utf-8 /DUNICODE /D_UNICODE ('/I'+(Join-Path $repo 'native/ts3wrap')) /Ferestart_host_checks.exe (Join-Path $PSScriptRoot 'restart_host_test.cpp')
 if($LASTEXITCODE -ne 0){throw 'Restart host test compilation failed'}
 & '.\restart_host_checks.exe' $OutDir
 if($LASTEXITCODE -ne 0){throw 'Restart host checks failed'}
} finally {Pop-Location}

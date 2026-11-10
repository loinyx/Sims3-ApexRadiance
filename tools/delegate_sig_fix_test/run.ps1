param([string]$OutDir=(Join-Path $env:TEMP 'ApexDelegateSigFixChecks'), [string]$Exe='')
# Delegate signature fix offline checks (check.cpp). Optional: -Exe (TS3W.exe or TS3.exe, read only).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /Fedelegate_sig_fix_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Delegate signature fix test compilation failed'}
 $runArgs=@()
 if($Exe){$runArgs+=@('--exe',$Exe)}
 & '.\delegate_sig_fix_checks.exe' @runArgs
 if($LASTEXITCODE -ne 0){throw 'Delegate signature fix checks failed'}
} finally {Pop-Location}

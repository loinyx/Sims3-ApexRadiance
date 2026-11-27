param([string]$OutDir=(Join-Path $env:TEMP 'ApexLotWaitChecks'), [string]$Exe='')
# Lot wait offline checks (check.cpp). Optional: -Exe (TS3W.exe, or a decrypted EA app TS3.exe image in file layout; read only).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNOMINMAX ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) ('/I'+$repo) ('/I'+(Join-Path $repo 'vcpkg_installed/x86-windows-static/include')) /Felot_wait_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Lot wait test compilation failed'}
 $runArgs=@()
 if($Exe){$runArgs+=@('--exe',$Exe)}
 & '.\lot_wait_checks.exe' @runArgs
 if($LASTEXITCODE -ne 0){throw 'Lot wait checks failed'}
} finally {Pop-Location}

param([string]$OutDir=(Join-Path $env:TEMP 'ApexSaveNameFixChecks'), [string]$Dll='', [string]$Patched='')
# Save name fix offline checks (check.cpp). Optional: -Dll (Sims3GameplaySystems.dll taken out of gameplay.package, read only) and
# -Patched (where the patched copy is written, to be decompiled).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /Fesave_name_fix_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Save name fix test compilation failed'}
 $runArgs=@()
 if($Dll){$runArgs+=@('--dll',$Dll)}
 if($Dll -and $Patched){$runArgs+=@('--out',$Patched)}
 & '.\save_name_fix_checks.exe' @runArgs
 if($LASTEXITCODE -ne 0){throw 'Save name fix checks failed'}
} finally {Pop-Location}

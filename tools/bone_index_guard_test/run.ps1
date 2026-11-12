param([string]$OutDir=(Join-Path $env:TEMP 'ApexBoneIndexGuardChecks'), [string]$Exe='')
# Bone index guard offline checks (check.cpp). Optional: -Exe (TS3W.exe or TS3.exe, read only).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNOMINMAX ('/I'+(Join-Path $repo 'framework')) ('/I'+$repo) ('/I'+(Join-Path $repo 'vcpkg_installed/x86-windows-static/include')) /Febone_index_guard_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Bone index guard test compilation failed'}
 $runArgs=@()
 if($Exe){$runArgs+=@('--exe',$Exe)}
 & '.\bone_index_guard_checks.exe' @runArgs
 if($LASTEXITCODE -ne 0){throw 'Bone index guard checks failed'}
} finally {Pop-Location}

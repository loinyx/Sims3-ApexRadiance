param([string]$OutDir=(Join-Path $env:TEMP 'ApexGroundShadeChecks'),[string]$Reference='',[int]$Reps=9)
# The lamp shade on lot ground and on the world grass around lots (features/ground_lamp_shadow.h): check.cpp times the lot
# build on a synthetic 40 m lot and checks a chunk made again over a changed lot's box against the whole chunk made again.
# -Reference <revision>: the header of that revision is compiled beside the current one and the lot build's results are
# compared bit for bit. Read-only: writes only the reference header and the compiler outputs to -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$defines=@()
if($Reference){
 $ref=& git -C $repo show ($Reference+':features/ground_lamp_shadow.h')
 if($LASTEXITCODE -ne 0){throw ('No ground_lamp_shadow.h at '+$Reference)}
 Set-Content -LiteralPath (Join-Path $OutDir 'ground_lamp_shadow_ref.h') -Value ($ref -join "`n") -Encoding UTF8
 $defines+='/DAPEX_SHADE_REF'
}
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /Oi /MT @defines ('/I'+$repo) ('/I'+(Join-Path $repo 'features')) ('/I'+$OutDir) /Feground_shade_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Ground shade check compilation failed'}
 & '.\ground_shade_checks.exe' --reps $Reps
 if($LASTEXITCODE -ne 0){throw 'Ground shade checks failed'}
} finally {Pop-Location}

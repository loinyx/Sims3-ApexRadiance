param([Parameter(Mandatory=$true)][string]$OpaqueShader,[Parameter(Mandatory=$true)][string]$CutoutShader,[string]$OutDir=(Join-Path $env:TEMP 'ApexMeshOcclusionChecks'),[switch]$Standard,[switch]$Development)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$source=Get-Content -LiteralPath (Join-Path $repo 'features/lot_light_bridge.cpp') -Raw
$begin=$source.IndexOf('bool ReadElement(')
$end=$source.IndexOf('// POD only (SEH): one wall per piece of the group',$begin)
if($begin -lt 0 -or $end -le $begin){throw 'Production mesh collector not found'}
Set-Content -LiteralPath (Join-Path $OutDir 'collector.inc') -Value $source.Substring($begin,$end-$begin) -Encoding utf8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 $flavor=@(if($Standard){}else{'/DAPEX_NO_DEV_TOOLS';'/DAPEX_MEMORY_TEST'})
 if($Development){$flavor+='/DAPEX_DEVELOPMENT_BUILD_FLAVOR'}
 & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT @flavor ('/I'+$repo) ('/I'+$OutDir) /Femesh_occlusion_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp') d3d9.lib d3dcompiler.lib user32.lib
 if($LASTEXITCODE -ne 0){throw 'Mesh occlusion fixture did not compile'}
 & '.\mesh_occlusion_checks.exe' (Resolve-Path -LiteralPath $OpaqueShader).Path (Resolve-Path -LiteralPath $CutoutShader).Path
 if($LASTEXITCODE -ne 0){throw 'Mesh occlusion checks failed'}
} finally {Pop-Location}

param([string]$OutDir=(Join-Path $env:TEMP 'ApexPostSceneChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$source=Get-Content -LiteralPath (Join-Path $repo 'features/picture.cpp') -Raw
$a=$source.IndexOf('bool EnsureSceneCopy(');$b=$source.IndexOf('void CopyScene(',$a)
if($a -lt 0 -or $b -le $a){throw 'Production scene-copy helper missing'}
Set-Content -LiteralPath (Join-Path $OutDir 'picture-scene-copy.inc') -Value $source.Substring($a,$b-$a) -Encoding utf8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 foreach($fixture in @('post_scene_check','scene_copy_check')) {
  & cl.exe /nologo /std:c++20 /EHsc /O2 /MT ('/I'+$OutDir) ('/I'+$repo) ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) ('/I'+(Join-Path $repo 'vcpkg_installed/x86-windows-static/include')) ('/Fe'+$fixture+'.exe') (Join-Path $PSScriptRoot ($fixture+'.cpp')) /link d3d9.lib user32.lib
  if($LASTEXITCODE){throw "$fixture compilation failed"}
  & (Join-Path $OutDir ($fixture+'.exe'))
  if($LASTEXITCODE){throw "$fixture failed"}
 }
} finally {Pop-Location}

param([string]$OutDir=(Join-Path $env:TEMP 'ApexStoryMapsChecks'), [string]$ReferenceSource)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir=(Resolve-Path -LiteralPath $OutDir).Path
$source=Get-Content -LiteralPath $(if($ReferenceSource){$ReferenceSource}else{Join-Path $repo 'features/level_light_share.cpp'}) -Raw
$start=$source.IndexOf('struct StoryMapSlot {')
$end=$source.IndexOf('// The drawn ambient against a room',$start)
if($start -lt 0 -or $end -lt $start){throw 'Production block missing'}
Set-Content -LiteralPath (Join-Path $OutDir 'production.h') -Value $source.Substring($start,$end-$start) -Encoding UTF8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try{
 $exe=Join-Path $OutDir 'story-maps.exe'
 & cl.exe /nologo /std:c++20 /EHa /W4 /O2 /MT /utf-8 ('/I'+$OutDir) ('/Fe:'+$exe) (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Story maps check compilation failed'}
 & $exe
 if($LASTEXITCODE -ne 0){throw 'Story maps check failed'}
}finally{Pop-Location}

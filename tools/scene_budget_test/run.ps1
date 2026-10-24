param([string]$OutDir=(Join-Path $env:TEMP 'ApexSceneBudgetChecks'), [string]$ReferenceSource)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir=(Resolve-Path -LiteralPath $OutDir).Path
$source=Get-Content -LiteralPath $(if($ReferenceSource){$ReferenceSource}else{Join-Path $repo 'features/scene_budget.cpp'}) -Raw
function Between([string]$from,[string]$to){
 $start=$source.IndexOf($from)
 if($start -lt 0){throw ('Production block missing: '+$from)}
 $end=$source.IndexOf($to,$start)
 if($end -lt $start){throw ('Production block end missing: '+$to)}
 $source.Substring($start,$end-$start)
}
$parts=@((Between 'void UpdateTail(' '// Room for `count`'),(Between 'uint32_t BudgetedDrain(' "// Scene::BeginFrame's CALL"))
Set-Content -LiteralPath (Join-Path $OutDir 'production.h') -Value ($parts -join "`n") -Encoding UTF8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try{
 foreach($variant in 0,1){
  $exe=Join-Path $OutDir ('scene-budget-'+$variant+'.exe')
  & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+$OutDir) ('/DPUBLIC_VARIANT='+$variant) ('/Fe:'+$exe) (Join-Path $PSScriptRoot 'check.cpp')
  if($LASTEXITCODE -ne 0){throw 'Scene budget checks compilation failed'}
  & $exe
  if($LASTEXITCODE -ne 0){throw 'Scene budget checks failed'}
 }
}finally{Pop-Location}

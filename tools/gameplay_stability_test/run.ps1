param([string]$OutDir=(Join-Path $env:TEMP 'ApexGameplayStabilityChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$source=Get-Content -LiteralPath (Join-Path $repo 'features/script_math.cpp') -Raw
$a=$source.IndexOf('using FnTypeGetObject =')
$b=$source.IndexOf('// SEH only:',$a)
$c=$source.IndexOf('void* __cdecl Hook_TypeGetObject(')
$d=$source.IndexOf('bool StartTypeCache(',$c)
if($a -lt 0 -or $b -le $a -or $c -lt 0 -or $d -le $c){throw 'Production type-cache functions not found'}
Set-Content -LiteralPath (Join-Path $OutDir 'script-types.inc') -Value ($source.Substring($a,$b-$a)+$source.Substring($c,$d-$c)) -Encoding utf8
$objects=Get-Content -LiteralPath (Join-Path $repo 'features/object_index.cpp') -Raw
$a=$objects.IndexOf('struct Entry {');$b=$objects.IndexOf('Entry* g_table',$a)
$c=$objects.IndexOf('bool Find(uint32_t');$d=$objects.IndexOf('void Store(Entry&',$c)
$e=$objects.IndexOf('Why ValidateRaw(');$f=$objects.IndexOf('Why Validate(',$e)
if($a -lt 0 -or $b -le $a -or $c -lt 0 -or $d -le $c -or $e -lt 0 -or $f -le $e){throw 'Production object-cache functions not found'}
Set-Content -LiteralPath (Join-Path $OutDir 'object-entry.inc') -Value $objects.Substring($a,$b-$a) -Encoding utf8
Set-Content -LiteralPath (Join-Path $OutDir 'object-cache.inc') -Value ($objects.Substring($c,$d-$c)+$objects.Substring($e,$f-$e)) -Encoding utf8
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try{
 & cl.exe /nologo /std:c++20 /utf-8 /EHsc /O2 /MT ('/I'+$OutDir) /Fescript_cache_checks.exe (Join-Path $PSScriptRoot 'script_cache.cpp')
 if($LASTEXITCODE){throw 'Script cache checks did not compile'}
 & '.\script_cache_checks.exe'
 if($LASTEXITCODE){throw 'Script cache checks failed'}
 & cl.exe /nologo /std:c++20 /utf-8 /EHsc /O2 /MT ('/I'+$OutDir) /Feobject_cache_checks.exe (Join-Path $PSScriptRoot 'object_cache.cpp')
 if($LASTEXITCODE){throw 'Object cache checks did not compile'}
 & '.\object_cache_checks.exe'
 if($LASTEXITCODE){throw 'Object cache checks failed'}
 & cl.exe /nologo /std:c++20 /utf-8 /EHsc /O2 /MT ('/I'+(Join-Path $repo 'features')) /Fepolicy_checks.exe (Join-Path $PSScriptRoot 'policy.cpp') (Join-Path $repo 'features/cas_tri_sort.cpp')
 if($LASTEXITCODE){throw 'Policy checks did not compile'}
 & '.\policy_checks.exe'
 if($LASTEXITCODE){throw 'Policy checks failed'}
}finally{Pop-Location}

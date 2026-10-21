param([string]$OutDir=(Join-Path $env:TEMP 'ApexMemoryOwnersChecks'), [string]$Dxvk='')
# Apex's own memory accounting (features/apex_memory.cpp, development flavour): heap counters, container estimates, owner
# slots and Direct3D 9 resource tracking on a real device. -Dxvk <d3d9.dll>: also run with that DLL (the game's DXVK) next
# to a second copy of the executable. Read-only: writes only the compiler outputs to -OutDir.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNOMINMAX ('/I'+$repo) ('/I'+(Join-Path $repo 'framework')) ('/I'+(Join-Path $repo 'features')) /Fememory_owners_checks.exe (Join-Path $PSScriptRoot 'check.cpp') /link psapi.lib user32.lib
 if($LASTEXITCODE -ne 0){throw 'Memory owners check compilation failed'}
 & '.\memory_owners_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Memory owners checks failed'}
 if($Dxvk){
  $dx=Join-Path $OutDir 'dxvk'
  New-Item -ItemType Directory -Path $dx -Force | Out-Null
  Copy-Item -LiteralPath (Join-Path $OutDir 'memory_owners_checks.exe') -Destination $dx -Force
  Copy-Item -LiteralPath $Dxvk -Destination (Join-Path $dx 'd3d9.dll') -Force
  Push-Location -LiteralPath $dx
  try { & '.\memory_owners_checks.exe'; if($LASTEXITCODE -ne 0){throw 'Memory owners checks failed on DXVK'} } finally {Pop-Location}
 }
} finally {Pop-Location}

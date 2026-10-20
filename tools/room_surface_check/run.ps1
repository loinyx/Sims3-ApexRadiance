param([Parameter(Mandatory=$true)][string]$Precomp,[string[]]$Captures=@(),[string]$OutDir=(Join-Path $env:TEMP 'ApexRoomSurfaceChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT /Feroom_surface_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp') d3d9.lib d3dcompiler.lib user32.lib
 if($LASTEXITCODE -ne 0){throw 'Room surface check compilation failed'}
 & '.\room_surface_checks.exe' (Resolve-Path -LiteralPath $Precomp).Path @($Captures | ForEach-Object {(Resolve-Path -LiteralPath $_).Path})
 if($LASTEXITCODE -ne 0){throw 'Room surface checks failed'}
} finally {Pop-Location}

# Compiles replay.cpp with MSVC and runs it on a RoomLampProbe dump (read-only).
param([Parameter(Mandatory = $true)][string]$Dump)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\..\.."
$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;$env:PATH"
. "$repo\tools\terrain_lighting_test\compiler_env.ps1"
Initialize-TerrainCompiler
$out = Join-Path $env:TEMP 'room_lamp_3d_replay'
New-Item -ItemType Directory -Force $out | Out-Null
# fresh exe name per run: a just-built exe can stay locked by the virus scanner
$exe = Join-Path $out "replay_$PID.exe"
$log = & cl.exe /nologo /O2 /EHsc /std:c++17 "$PSScriptRoot\replay.cpp" "/Fe$exe" "/Fo$out\\"
if ($LASTEXITCODE -ne 0) { $log; throw 'compile failed' }
& $exe $Dump
Remove-Item $exe -ErrorAction SilentlyContinue

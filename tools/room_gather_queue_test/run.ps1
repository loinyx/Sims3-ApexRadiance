param([string]$OutDir=(Join-Path $env:TEMP 'ApexRoomGatherQueueChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
$source=Get-Content -LiteralPath (Join-Path $repo 'features/level_light_share.cpp') -Raw
$blocks=@()
foreach($name in @('RoomGatherQueue::Membership OutdoorQueueMembership','bool RestoreOutdoorGatherTry')) {
    $m=[regex]::Match($source, '(?ms)^'+[regex]::Escape($name)+'\([^\n]*\) \{.*?^\}')
    if(-not $m.Success) { throw ('Native queue block missing: '+$name) }
    $blocks += $m.Value
}
[IO.File]::WriteAllText((Join-Path $OutDir 'native_queue_fixture.h'), ($blocks -join "`n"), [Text.UTF8Encoding]::new($false))
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT ('/I'+$OutDir) /Feroom_gather_queue_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Room gather queue test compilation failed' }
    & '.\room_gather_queue_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Room gather queue checks failed' }
} finally { Pop-Location }

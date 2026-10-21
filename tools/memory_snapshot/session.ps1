param(
 [ValidateRange(2,60)][int]$Samples=6,
 [ValidateRange(15,600)][int]$IntervalSeconds=60,
 [string]$Label='gameplay-route',
 [string]$OutputRoot=(Join-Path $PSScriptRoot 'RAM-Snapshots'),
 [switch]$ResidentPages
)
$ErrorActionPreference='Stop'
$games=@(Get-Process TS3W -ErrorAction SilentlyContinue)
if($games.Count -ne 1){throw 'Open one TS3W game before starting the session.'}
$gameId=$games[0].Id
$started=$games[0].StartTime.ToUniversalTime().Ticks
$capture=Join-Path $PSScriptRoot 'Analisar-RAM-TS3.ps1'
if(!(Test-Path -LiteralPath $capture)){$capture=Join-Path $PSScriptRoot 'capture.ps1'}
$safeLabel=[regex]::Replace($Label,'[^a-zA-Z0-9_-]','_')
$session=Join-Path $OutputRoot ("session-{0}-{1}-{2}" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'),$gameId,$safeLabel)
if(Test-Path -LiteralPath $session){throw 'Session folder exists.'}
New-Item -ItemType Directory -Path $session | Out-Null
for($i=0;$i -lt $Samples;$i++){
 $game=Get-Process -Id $gameId -ErrorAction SilentlyContinue
 if(!$game -or $game.StartTime.ToUniversalTime().Ticks -ne $started){Write-Warning 'Game session ended; collection stopped.';break}
 Write-Host ("Snapshot {0}/{1}" -f ($i+1),$Samples)
 & $capture -ProcessId $gameId -Label ("{0}-{1:D2}" -f $safeLabel,$i) -OutputRoot $session -ResidentPages:$ResidentPages
 if($i+1 -lt $Samples){Start-Sleep -Seconds $IntervalSeconds}
}
& (Join-Path $PSScriptRoot 'compare.ps1') -SnapshotRoot $session
Write-Output $session

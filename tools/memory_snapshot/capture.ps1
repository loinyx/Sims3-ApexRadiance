param(
 [int]$ProcessId=0,
 [string]$Label='gameplay',
 [string]$OutputRoot=(Join-Path $PSScriptRoot 'RAM-Snapshots'),
 [switch]$ResidentPages
)
$ErrorActionPreference='Stop'
$collector=Join-Path $PSScriptRoot 'ApexMemorySnapshot.exe'
if(!(Test-Path -LiteralPath $collector)){throw 'ApexMemorySnapshot.exe must be beside this script.'}
if(!$ProcessId){
 $games=@(Get-Process TS3W -ErrorAction SilentlyContinue)
 if($games.Count -ne 1){throw 'Open one TS3W game, or select its ProcessId explicitly.'}
 $ProcessId=$games[0].Id
}
$targetProcess=Get-Process -Id $ProcessId
if($targetProcess.ProcessName -ne 'TS3W'){throw 'This capture script only accepts TS3W.'}
$safeLabel=[regex]::Replace($Label,'[^a-zA-Z0-9_-]','_')
if(!$safeLabel){$safeLabel='gameplay'}
$folder=Join-Path $OutputRoot ("{0}-{1}-{2}" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'),$ProcessId,$safeLabel)
$collectorArgs=@([string]$ProcessId,$folder)
if($ResidentPages){$collectorArgs+='--resident'}
$matchedApex=$null
foreach($module in $targetProcess.Modules){
 if($module.ModuleName -notlike '*Apex*.asi'){continue}
 $hash=(Get-FileHash -LiteralPath $module.FileName -Algorithm SHA256).Hash.ToLowerInvariant()
 $layouts=@{
  'e8530ddbe31f6f9c853ca7ba43e10986521c7a85f888e7edd982eb526be213a0'=@{rva=0x75d64c;version='2.13.2-test.3'}
  'f8df04b0606d3128e6d6154790d8bb46f35a7efcd95baf7960e568e7be1174cf'=@{rva=0x7687bc;version='2.13.2-test.9'}
  '23ccc14fee938bbeee09905b89b5d83d8e4a76d642bc34cd0085f48152113c52'=@{rva=0x76a7c4;version='2.13.2-test.10'}
  '1473b84f29aa434bb9f4fadc00a62805105c52c64b6f666c8425e79ef29df544'=@{rva=0x76b7c4;version='2.13.2-test.11'}
 }
 if($layouts.ContainsKey($hash)){
  $layout=$layouts[$hash]
  $globalAddress=$module.BaseAddress.ToInt64()+$layout.rva
  $collectorArgs+=@('--resource-global',('0x{0:X8}' -f $globalAddress))
  $matchedApex=$layout.version
  break
 }
}
& $collector @collectorArgs
if($LASTEXITCODE -ne 0){throw "Snapshot failed with exit code $LASTEXITCODE"}
@{label=$Label;captured_at=(Get-Date -Format o);process_started_at=$targetProcess.StartTime.ToUniversalTime().ToString('o');pid=$ProcessId;apex_resource_layout=$matchedApex;resident_pages=[bool]$ResidentPages;writes_to_game=$false} |
 ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'Capture.json') -Encoding utf8
$allocations=Import-Csv -LiteralPath (Join-Path $folder 'Allocations.csv')
$allocations | Select-Object -First 10 allocation_base,type,@{n='Committed_MiB';e={[math]::Round([double]$_.committed_va_bytes/1MB,1)}},mapped_file | Format-Table
Import-Csv -LiteralPath (Join-Path $folder 'EngineCounters.csv') | Format-Table
Write-Output $folder

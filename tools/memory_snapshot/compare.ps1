param([Parameter(Mandatory=$true)][string]$SnapshotRoot)
$ErrorActionPreference='Stop'
$rows=@(foreach($file in (Get-ChildItem -LiteralPath $SnapshotRoot -Filter Summary.json -Recurse | Sort-Object FullName)){
 $s=Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
 $folder=$file.DirectoryName
 $capturePath=Join-Path $folder 'Capture.json'
 $capture=if(Test-Path -LiteralPath $capturePath){Get-Content -LiteralPath $capturePath -Raw | ConvertFrom-Json}else{$null}
 $engine=@(Import-Csv -LiteralPath (Join-Path $folder 'EngineCounters.csv'))
 $mono=@($engine | Where-Object {$_.category -eq 'mono_boehm_heap' -and $_.valid -eq 'true'})
 $primary=@($engine | Where-Object {$_.category -eq 'native_primary_large' -and $_.valid -eq 'true'})
 $runtime=@($engine | Where-Object {$_.category -eq 'runtime_native_large' -and $_.valid -eq 'true'})
 $cachePath=Join-Path $folder 'ResourceCaches.csv'
 $caches=if(Test-Path -LiteralPath $cachePath){@(Import-Csv -LiteralPath $cachePath)}else{@()}
 $cache0=@($caches | Where-Object {$_.valid -eq 'true' -and $_.cache_index -eq '0'})
 $cache1=@($caches | Where-Object {$_.valid -eq 'true' -and $_.cache_index -eq '1'})
 [pscustomobject][ordered]@{
  folder=$file.Directory.Name;pid=$s.pid;process_started_at=$capture.process_started_at;captured_at=$capture.captured_at;label=$capture.label
  scan_complete=$s.scan_complete;elapsed_ms=$s.elapsed_ms
  private_bytes=$(if($s.process_valid){$s.private_bytes}else{$null})
  working_set_bytes=$(if($s.process_valid){$s.working_set_bytes}else{$null})
  free_va_bytes=$s.free_va_bytes;largest_free_va_bytes=$s.largest_free_va_bytes
  free_below_2g_bytes=$s.free_below_2g_bytes;largest_below_2g_bytes=$s.largest_below_2g_bytes
  mono_heap_bytes=$(if($mono.Count -eq 1){[long]$mono[0].bytes}else{$null})
  mono_free_bytes=$(if($mono.Count -eq 1){[long]$mono[0].free_bytes}else{$null})
  mono_accounted_used_bytes=$(if($mono.Count -eq 1){[long]$mono[0].bytes-[long]$mono[0].free_bytes}else{$null})
  primary_large_bytes=$(if($primary.Count -eq 1){[long]$primary[0].bytes}else{$null})
  runtime_large_bytes=$(if($runtime.Count -eq 1){[long]$runtime[0].bytes}else{$null})
  native_globals_alias=$(if($primary.Count -eq 1 -and $runtime.Count -eq 1){$primary[0].address -eq $runtime[0].address}else{$null})
  primary_top_down=$(if($primary.Count -eq 1){[int]$primary[0].top_down_flag}else{$null})
  cache0_idle_accounting_bytes=$(if($cache0.Count -eq 1){[long]$cache0[0].idle_accounting_bytes}else{$null})
  cache1_idle_accounting_bytes=$(if($cache1.Count -eq 1){[long]$cache1[0].idle_accounting_bytes}else{$null})
 }
})
if(!$rows.Count){throw 'No snapshot summaries found.'}
$rows | Export-Csv -LiteralPath (Join-Path $SnapshotRoot 'Timeline.csv') -NoTypeInformation -Encoding utf8
$notes=@('Sequential snapshots are approximate. Engine counters overlap process memory and must not be added to it.',
 'Unknown counter values remain blank. Mono used accounting is not exact live-object memory.',
 'Growth alone does not establish a leak. Compare the same route, idle interval and save phase.',
 'Snapshot work can affect timing; this session is for memory analysis, not FPS benchmarking.')
$groups=@($rows | Where-Object {$_.process_started_at} | Group-Object pid,process_started_at)
$deltas=@(foreach($group in $groups){
 $valid=@($group.Group | Where-Object {$_.scan_complete -and $null -ne $_.private_bytes})
 if($valid.Count -lt 2){continue}
 $first=$valid[0];$last=$valid[-1]
 $delta=[ordered]@{pid=$first.pid;samples=$valid.Count;first=$first.folder;last=$last.folder}
 foreach($field in @('private_bytes','working_set_bytes','free_va_bytes','largest_free_va_bytes','mono_heap_bytes','mono_accounted_used_bytes','primary_large_bytes','runtime_large_bytes','cache0_idle_accounting_bytes','cache1_idle_accounting_bytes')){
  $delta[$field+'_delta']=$(if($null -ne $first.$field -and $null -ne $last.$field){[long]$last.$field-[long]$first.$field}else{$null})
 }
 [pscustomobject]$delta
})
@{snapshots=$rows.Count;deltas=$deltas;limitations=$notes} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $SnapshotRoot 'Comparison.json') -Encoding utf8
$rows | Format-Table label,pid,private_bytes,mono_heap_bytes,primary_large_bytes,largest_free_va_bytes

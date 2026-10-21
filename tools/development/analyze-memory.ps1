param([Parameter(Mandatory=$true)][string]$Csv)
$ErrorActionPreference='Stop'
$rows=@(Import-Csv -LiteralPath $Csv)
if(!$rows.Count){throw 'The CSV contains no samples.'}
$valid=@($rows | Where-Object {$_.process_valid -eq 'true'})
$scans=@($rows | Where-Object {$_.scan_complete -eq 'true'})
function MiB($value){[Math]::Round([double]$value/1MB,1)}
Write-Output "Samples: $($rows.Count); valid process samples: $($valid.Count); complete address scans: $($scans.Count)"
if($valid.Count){
 $first=$valid[0];$last=$valid[-1]
 $peak=($valid | ForEach-Object {[double]$_.private_bytes} | Measure-Object -Maximum).Maximum
 $peakWs=($valid | ForEach-Object {[double]$_.working_set_bytes} | Measure-Object -Maximum).Maximum
 Write-Output "Private memory: $(MiB $first.private_bytes) -> $(MiB $last.private_bytes) MiB; peak $(MiB $peak) MiB"
 Write-Output "Peak sampled physical working set: $(MiB $peakWs) MiB"
 $minutes=([double]$last.uptime_ms-[double]$first.uptime_ms)/60000
 if($minutes -gt 0){Write-Output "Net private memory change per minute: $([Math]::Round((([double]$last.private_bytes-[double]$first.private_bytes)/1MB)/$minutes,2)) MiB/min; this is not proof of a leak."}
}
if($scans.Count){
 $small=($scans | ForEach-Object {[double]$_.largest_free_va_bytes} | Measure-Object -Minimum).Minimum
 $low=($scans | ForEach-Object {[double]$_.largest_below_2g_bytes} | Measure-Object -Minimum).Minimum
 Write-Output "Smallest sampled largest free address block: $(MiB $small) MiB; below 2 GiB: $(MiB $low) MiB"
}
Write-Output 'Compare sessions at the same gameplay milestones. RAM, committed memory and virtual address space describe different limits. This report does not identify allocation owners or free memory.'

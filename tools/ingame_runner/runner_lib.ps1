# Pure helpers of the in-game runner (no game, no files outside the arguments): job expansion, TOML switches, log
# checks, frame statistics and the results page. Dot-sourced by run.ps1 and selftest.ps1. Windows PowerShell 5.1 and
# PowerShell 7. Adapted in part from sims3-modernization tools/harness (MIT, see LICENSE-sims3-modernization).

# Patterns that fail every load: a feature that refused to start or turned itself off after a verification difference.
$script:DefaultFailPatterns = @(
    '\[CRITICAL\]',
    '\[Patches\] .* did not start',
    'Verification mismatch',
    'turn(s|ed) itself off',
    'Result differs from the game',
    'Safety check failed'
)

# The mode step's targets and the in-world sub state each one waits for (agent set_mode).
$script:ModeStates = [ordered]@{ build = 'BuildModeState'; buy = 'BuyModeState'; cas = 'CASFullModeState'; edittown = 'EditTownState'; live = 'LiveModeState' }
# A wait's `until` field: the repro command whose status holds the count.
$script:UntilFields = @{ jumps = 'camera_jumps'; loops = 'camera_jumps'; outfitChanges = 'camera_jumps'; spawned = 'camera_jumps'; rounds = 'vfx_burst'; started = 'vfx_burst' }
function Get-UntilCommand([string]$Field) {
    if (-not $script:UntilFields.ContainsKey($Field)) { throw "until: '$Field' is not one of $($script:UntilFields.Keys -join ', ')" }
    return $script:UntilFields[$Field]
}
function Format-Until($Until) { $p = @($Until.PSObject.Properties)[0]; return "$($p.Value) $($p.Name)" }
# Every step run.ps1 knows.
$script:KnownSteps = @('speed', 'wait', 'profile', 'camera', 'camera_pan', 'save', 'reload', 'camera_jumps', 'vfx_burst', 'mode', 'stop_repros')

function ConvertTo-TomlLiteral($Value) {
    if ($Value -is [bool]) { if ($Value) { return 'true' } else { return 'false' } }
    if ($Value -is [int] -or $Value -is [long]) { return [string]$Value }
    if ($Value -is [double] -or $Value -is [decimal] -or $Value -is [single]) { return ([double]$Value).ToString('R', [Globalization.CultureInfo]::InvariantCulture) }
    if ($Value -is [string]) { return '"' + ($Value -replace '\\', '\\' -replace '"', '\"') + '"' }
    throw "Unsupported TOML value: $Value"
}

# Sets key = value inside [Table] of a TOML text as Apex writes it (standard tables, one key per line). A missing key
# is added under the table header, a missing table at the end. Other tables and comments stay as they are.
function Set-TomlValue([string]$Text, [string]$Table, [string]$Key, $Value) {
    $literal = ConvertTo-TomlLiteral $Value
    $lines = New-Object System.Collections.Generic.List[string]
    if ($Text) { foreach ($l in ($Text -split "`r?`n")) { $lines.Add($l) } }
    while ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    $header = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -eq "[$Table]") { $header = $i; break }
    }
    if ($header -lt 0) {
        if ($lines.Count -gt 0) { $lines.Add('') }
        $lines.Add("[$Table]")
        $lines.Add("$Key = $literal")
    } else {
        $end = $lines.Count
        for ($i = $header + 1; $i -lt $lines.Count; $i++) { if ($lines[$i].TrimStart().StartsWith('[')) { $end = $i; break } }
        $done = $false
        for ($i = $header + 1; $i -lt $end; $i++) {
            if ($lines[$i] -match ('^\s*"?' + [regex]::Escape($Key) + '"?\s*=')) { $lines[$i] = "$Key = $literal"; $done = $true; break }
        }
        if (-not $done) { $lines.Insert($header + 1, "$Key = $literal") }
    }
    return (($lines -join "`r`n") + "`r`n")
}

# Overrides: { "patches.ResourceLookupCache": { "enabled": false }, ... } as parsed by ConvertFrom-Json.
function Set-TomlOverrides([string]$Text, $Overrides) {
    if ($null -eq $Overrides) { return $Text }
    foreach ($table in $Overrides.PSObject.Properties) {
        foreach ($kv in $table.Value.PSObject.Properties) { $Text = Set-TomlValue $Text $table.Name $kv.Name $kv.Value }
    }
    return $Text
}

# The loads of one job: one variant, or every variant in an ABBA order (A B B A for two variants), so a drift over the
# session (warm file cache, background load) does not favour the variant that always runs second.
function Get-JobLoads($Job, [string]$Variant, [switch]$AB) {
    $names = @($Job.variants.PSObject.Properties | ForEach-Object { $_.Name })
    if ($names.Count -eq 0) { throw "Job $($Job.id) has no variants" }
    if ($AB) {
        if ($Job.order) { $order = @($Job.order) } else { $order = @($names + ($names[($names.Count - 1)..0])) }
    } elseif ($Variant) {
        $order = @($Variant)
    } else {
        $order = @($names[$names.Count - 1]) # the last variant is the feature on, its default
    }
    foreach ($n in $order) { if ($names -notcontains $n) { throw "Job $($Job.id) has no variant '$n' (variants: $($names -join ', '))" } }
    $i = 0
    foreach ($n in $order) { $i++; [pscustomobject]@{ Job = $Job; Variant = $n; Index = $i; Name = ('{0}-{1}-{2}' -f $Job.id, $i, $n) } }
}

# A job's steps; without a steps list: set the speed, then one measured wait of `minutes`.
function Get-JobSteps($JobDef) {
    if ($JobDef.steps) { return @($JobDef.steps) }
    $speed = if ($JobDef.speed) { $JobDef.speed } else { 'normal' }
    $minutes = if ($JobDef.minutes) { $JobDef.minutes } else { 10 }
    return @([pscustomobject]@{ do = 'speed'; speed = $speed }, [pscustomobject]@{ do = 'wait'; minutes = $minutes; measure = $true })
}

# Pass rule on ApexRadiance_LOG.txt: every expect pattern matches a line; no fail pattern matches any line.
function Test-ApexLog([string[]]$Lines, [string[]]$Expect, [string[]]$Fail) {
    $missing = @($Expect | Where-Object { $_ } | Where-Object { $p = $_; -not ($Lines | Where-Object { $_ -match $p } | Select-Object -First 1) })
    $hits = @()
    foreach ($p in @($Fail | Where-Object { $_ })) { $hits += @($Lines | Where-Object { $_ -match $p }) }
    [pscustomobject]@{ Ok = ($missing.Count -eq 0 -and $hits.Count -eq 0); Missing = $missing; Failures = @($hits | Select-Object -Unique) }
}

function Get-Percentile([double[]]$Sorted, [double]$P) {
    if ($Sorted.Count -eq 0) { return $null }
    $i = [math]::Min($Sorted.Count - 1, [math]::Max(0, [int][math]::Ceiling($P / 100 * $Sorted.Count) - 1))
    return $Sorted[$i]
}

function ConvertTo-Number($Text) {
    if ($null -eq $Text -or $Text -eq '' -or $Text -eq 'na') { return [double]::NaN }
    return [double]::Parse([string]$Text, [Globalization.CultureInfo]::InvariantCulture)
}

# Frame statistics from the frame-times.js CSV (epochMs,qpcMs,frameMs,workMs,fg, stage columns). Without -AllFrames only
# frames with the game window in front count: the game sleeps 10 ms per loop while another window is in front unless the
# game's own NoInactiveIdle setting is on (run.ps1 sets it), so with it every frame counts.
function Get-FrameStats([string]$CsvPath, [switch]$AllFrames) {
    if (-not (Test-Path -LiteralPath $CsvPath)) { return $null }
    $rows = @(Import-Csv -LiteralPath $CsvPath)
    if ($rows.Count -lt 2) { return $null }
    return Get-FrameStatsOf $rows -AllFrames:$AllFrames
}

function Get-FrameStatsOf($Rows, [int]$Total = 0, [switch]$AllFrames) {
    $rows = @($Rows)
    if (-not $Total) { $Total = $rows.Count }
    $fg = @($rows | Where-Object { $_.fg -eq '1' })
    $use = if ($AllFrames) { $rows } else { $fg }
    $ms = [double[]]@($use | ForEach-Object { ConvertTo-Number $_.frameMs } | Sort-Object)
    if ($ms.Count -eq 0) { return [pscustomobject]@{ Frames = 0; FocusedShare = 0 } }
    $totalMin = ($ms | Measure-Object -Sum).Sum / 60000
    [pscustomobject]@{
        Frames = $ms.Count
        FocusedShare = [math]::Round($fg.Count / $Total, 3)
        Fps = [math]::Round($ms.Count / ($totalMin * 60), 1)
        P50 = [math]::Round((Get-Percentile $ms 50), 2)
        P95 = [math]::Round((Get-Percentile $ms 95), 2)
        P99 = [math]::Round((Get-Percentile $ms 99), 2)
        Max = [math]::Round($ms[$ms.Count - 1], 2)
        Over50PerMin = [math]::Round(@($ms | Where-Object { $_ -gt 50 }).Count / $totalMin, 2)
    }
}

# ------------------------------------------------------------------ real hitches only
# A measured result holds only what a player would see. One classifier for slow sim iterations, publish gaps (visible
# freezes), seconds rows and frames; every excluded point is counted by reason instead. All times are QueryPerformanceCounter
# ms: the profilers stamp with it, and run.ps1 stamps its agent commands and machine samples with the same counter.
#   first minute   the first 60 seconds rows of the profile (save load settling, profiler start-up)
#   profiler edge  the last 2 s before the last row (profiler stop)
#   harness step   a job step (speed, camera, camera_pan, save, reload) from its start to 10 s after it returns, and any
#                  iteration of >= 1 s with no simulation step (a save or load); game cost of the scenario, reported apart
#   agent call     a test agent call outside a step (dialog and speed upkeep, 200 ms either side): it runs on the sim thread
#   render-paced   a publish gap with no sim iteration >= 50 ms inside: the screen waited on the render side (another
#                  window in front and the game's inactive-window sleep, or a frame limiter that signals the sim once per
#                  presented frame). With NoInactiveIdle on, a gap over >= 2 iterations without a publish stays real
# There are no sampler windows: the runner never samples stacks (a sampler suspends the sim thread and slows the main
# thread too), so measured loads need no sampler exclusion.
$script:ExclusionReasons = @('first minute', 'profiler edge', 'harness step', 'agent call', 'render-paced')
$script:FreezeThresholds = @(100, 300, 500, 1000, 2000)
$script:SimMs = 50

function Read-Jsonl([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    return @(Get-Content -LiteralPath $Path | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json })
}

# Windows from the profile's span and the runner's commands ({ label, kind = step|call, t0, t1 }), sorted by start.
function Get-ExclusionWindows([double]$FirstMinuteEnd, [double]$LastRow, $Commands) {
    $w = @([pscustomobject]@{ A = [double]::NegativeInfinity; B = $FirstMinuteEnd; Why = 'first minute'; What = 'load' },
           [pscustomobject]@{ A = $LastRow - 2000; B = [double]::PositiveInfinity; Why = 'profiler edge'; What = 'profiler stop' })
    foreach ($c in $Commands) { # not @($Commands): array-wrapping run.ps1's List[object] throws "Argument types do not match"
        if ($c.kind -eq 'step') { $w += [pscustomobject]@{ A = [double]$c.t0; B = [double]$c.t1 + 10000; Why = 'harness step'; What = $c.label } }
        else { $w += [pscustomobject]@{ A = [double]$c.t0 - 200; B = [double]$c.t1 + 200; Why = 'agent call'; What = $c.label } }
    }
    return @($w | Sort-Object A)
}

# The first-ranked window overlapping each item ($null = real). Items ({A, B}) and windows sorted by A: one sweep, so a
# 10-minute frame log stays fast in Windows PowerShell.
function Get-WindowHits([object[]]$Items, [object[]]$Wins) {
    $rank = @{}; for ($i = 0; $i -lt $script:ExclusionReasons.Count; $i++) { $rank[$script:ExclusionReasons[$i]] = $i }
    $out = New-Object object[] $Items.Count
    $active = New-Object System.Collections.Generic.List[object]
    $j = 0
    for ($i = 0; $i -lt $Items.Count; $i++) {
        $x = $Items[$i]
        while ($j -lt $Wins.Count -and $Wins[$j].A -lt $x.B) { $active.Add($Wins[$j]); $j++ }
        $best = $null
        for ($k = $active.Count - 1; $k -ge 0; $k--) {
            $w = $active[$k]
            if ($w.B -le $x.A) { $active.RemoveAt($k); continue }
            if ($null -eq $best -or $rank[$w.Why] -lt $rank[$best.Why]) { $best = $w }
        }
        $out[$i] = $best
    }
    return , $out
}

# Index of the seconds row holding each time (first row with T >= t; -1 after the last). Times ascending.
function Get-RowIndex([double[]]$Times, [object[]]$Rows) {
    $out = New-Object int[] $Times.Count
    $r = 0
    for ($i = 0; $i -lt $Times.Count; $i++) {
        while ($r -lt $Rows.Count -and $Rows[$r].T -lt $Times[$i]) { $r++ }
        $out[$i] = if ($r -lt $Rows.Count) { $r } else { -1 }
    }
    return , $out
}

function Add-Excluded([hashtable]$Table, [string]$Key, [double]$Ms, [double]$Secs = 0) {
    if (-not $Table.ContainsKey($Key)) { $Table[$Key] = [pscustomobject]@{ N = 0; Ms = 0.0; N300 = 0; N1000 = 0; Max = 0.0; Secs = 0.0; Freezes300 = 0 } }
    $r = $Table[$Key]
    if ($Ms) { $r.N++; $r.Ms += $Ms; if ($Ms -ge 300) { $r.N300++ }; if ($Ms -ge 1000) { $r.N1000++ }; if ($Ms -gt $r.Max) { $r.Max = $Ms } }
    $r.Secs += $Secs
}

# nearest-rank percentile when only the values >= 50 ms are known ($Tail) out of $Total: $null = below 50 ms
function Get-TailPercentile([double[]]$Tail, [int]$Total, [double]$P) {
    if ($Total -le 0) { return $null }
    $s = [double[]]@($Tail | Sort-Object)
    $i = [math]::Max(0, [int][math]::Ceiling($P / 100 * $Total) - 1)
    $lo = $Total - $s.Count
    if ($i -ge $lo) { return $s[$i - $lo] }
    return $null
}

# Main-loop stages of a frames.csv row (frame-times.js), exclusive: services and capture lose the consume and render
# callback nested in them. Unhooked stages (`na`) are left out; their time is in other.
$script:FrameStages = [ordered]@{ 'load state' = 'loadMs'; 'idle sleep' = 'idleMs'; 'services' = 'svcMs'; 'consume' = 'consumeMs';
    'video capture' = 'captureMs'; 'render callback' = 'rcbMs'; 'begin frame' = 'beginMs'; 'draw' = 'drawMs'; 'present' = 'presentMs';
    'after present' = 'postMs'; 'end-frame job wait' = 'endMs'; 'other' = 'otherMs' }
function Get-FrameStageMs($Row) {
    $st = [ordered]@{}
    foreach ($k in $script:FrameStages.Keys) {
        $v = ConvertTo-Number $Row.($script:FrameStages[$k])
        if ($k -eq 'services') { $c = ConvertTo-Number $Row.consumeMs; if (-not [double]::IsNaN($c)) { $v -= $c } }
        if ($k -eq 'video capture') { $c = ConvertTo-Number $Row.rcbMs; if (-not [double]::IsNaN($c)) { $v -= $c } }
        if (-not [double]::IsNaN($v)) { $st[$k] = [math]::Max(0, $v) }
    }
    return $st
}

# Frames of >= $Ms: how many each stage led (its longest exclusive stage), the stage ms summed, and the main-loop services
# (frame-times.js svc1..svc3: each frame's three longest) by ms. Empty when the frames.csv has no stage columns.
function Get-SlowFrames($Frames, [double]$Ms = 100) {
    $slow = @($Frames | Where-Object { $_.Ms -ge $Ms -and $null -ne $_.Row.PSObject.Properties['svcMs'] })
    $lead = @{}; $stageMs = @{}; $svc = @{}; $svcLead = @{}
    foreach ($f in $slow) {
        $st = Get-FrameStageMs $f.Row
        $top = $null
        foreach ($k in $st.Keys) { $stageMs[$k] = [double]$stageMs[$k] + $st[$k]; if ($null -eq $top -or $st[$k] -gt $st[$top]) { $top = $k } }
        if ($top) { $lead[$top] = [int]$lead[$top] + 1 }
        foreach ($i in 1, 2, 3) {
            $name = $f.Row."svc$i"; $v = ConvertTo-Number $f.Row."svc${i}Ms"
            if ($name -and $name -ne 'na' -and -not [double]::IsNaN($v)) { $svc[$name] = [double]$svc[$name] + $v; if ($i -eq 1) { $svcLead[$name] = [int]$svcLead[$name] + 1 } }
        }
    }
    [pscustomobject]@{
        N = $slow.Count; Ms = $Ms
        Stages = @($stageMs.Keys | Sort-Object { - $stageMs[$_] } | ForEach-Object { [pscustomobject]@{ Stage = $_; Lead = [int]$lead[$_]; Ms = [math]::Round($stageMs[$_]) } })
        Services = @($svc.Keys | Sort-Object { - $svc[$_] } | Select-Object -First 5 | ForEach-Object { [pscustomobject]@{ Service = $_; Lead = [int]$svcLead[$_]; Ms = [math]::Round($svc[$_]) } })
    }
}

# One measured wait: <Base>-seconds.csv / -hitches.jsonl / -freezes.jsonl (stutter-profile.js), <Base>-frames.csv
# (frame-times.js), the runner's $Commands and $Machine samples ({ t, game, other }: % of the whole machine). Returns the
# real numbers, the excluded ones by reason, the harness commands' own cost and why the whole load is invalid, if it is.
function Get-Measurement([string]$Base, [double]$Seconds, $Commands, $Machine, [switch]$NoInactiveIdle, [string]$Crash) {
    $rows = @(); $hitches = @(); $gaps = @(); $frames = @()
    if (Test-Path -LiteralPath "$Base-seconds.csv") {
        $prev = $null
        $rows = @(Import-Csv -LiteralPath "$Base-seconds.csv" | ForEach-Object {
            $t = ConvertTo-Number $_.t
            [pscustomobject]@{ A = $(if ($null -ne $prev) { $prev } else { $t - 1000 }); B = $t; T = $t; Iters = ConvertTo-Number $_.iters; Pubs = ConvertTo-Number $_.publishes }
            $prev = $t
        })
        $hitches = @(Read-Jsonl "$Base-hitches.jsonl" | ForEach-Object { [pscustomobject]@{ A = $_.t - $_.wallMs; B = [double]$_.t; Wall = [double]$_.wallMs; Steps = $_.steps; N = $_.n } })
        $gaps = @(Read-Jsonl "$Base-freezes.jsonl" | ForEach-Object { [pscustomobject]@{ A = [double]$_.t0; B = [double]$_.t1; Gap = [double]$_.gapMs; Iters = [int]$_.iters; N = $_.n } })
    }
    if (Test-Path -LiteralPath "$Base-frames.csv") {
        $frames = @(Import-Csv -LiteralPath "$Base-frames.csv" | ForEach-Object { $q = ConvertTo-Number $_.qpcMs; $ms = ConvertTo-Number $_.frameMs; [pscustomobject]@{ A = $q - $ms; B = $q; Ms = $ms; Row = $_ } })
    }
    if (-not $rows.Count -and $frames.Count -lt 2) { return $null }
    if ($rows.Count) { $firstMin = $rows[[math]::Min(59, $rows.Count - 1)].T; $last = $rows[$rows.Count - 1].T; $spanS = $rows.Count }
    else { $firstMin = $frames[0].B + 60000; $last = $frames[$frames.Count - 1].B; $spanS = ($last - $frames[0].B) / 1000 }
    $wins = Get-ExclusionWindows $firstMin $last $Commands
    $excl = @{}; $harness = @{}

    # seconds rows: an excluded row leaves the real minutes, its iterations and its publishes
    $rowWhy = Get-WindowHits $rows $wins
    $keptRows = 0; $iters = 0; $pubs = 0
    for ($i = 0; $i -lt $rows.Count; $i++) {
        if ($rowWhy[$i]) { Add-Excluded $excl $rowWhy[$i].Why 0 1 } else { $keptRows++; $iters += $rows[$i].Iters; if (-not [double]::IsNaN($rows[$i].Pubs)) { $pubs += $rows[$i].Pubs } }
    }

    # slow iterations: own window, a save or load, else their row's reason
    $hHit = Get-WindowHits $hitches $wins
    $hRow = Get-RowIndex ([double[]]@($hitches | ForEach-Object { $_.B })) $rows
    $real = New-Object System.Collections.Generic.List[double]
    $byN = @{}
    for ($i = 0; $i -lt $hitches.Count; $i++) {
        $h = $hitches[$i]
        if ($null -ne $h.N) { $byN[[long]$h.N] = $h.Wall }
        $why = $null; $what = $null
        if ($hHit[$i]) { $why = $hHit[$i].Why; if ($why -in 'harness step', 'agent call') { $what = $hHit[$i].What } }
        elseif ($h.Steps -eq 0 -and $h.Wall -ge 1000) { $why = 'harness step'; $what = 'save or load (iteration without a sim step)' }
        elseif ($hRow[$i] -lt 0) { $why = 'profiler edge' }
        elseif ($rowWhy[$hRow[$i]]) { $why = $rowWhy[$hRow[$i]].Why }
        if (-not $why) { $real.Add($h.Wall); continue }
        Add-Excluded $excl $why $h.Wall
        if ($what) { Add-Excluded $harness $what $h.Wall }
        if ($hRow[$i] -ge 0 -and -not $rowWhy[$hRow[$i]]) { $iters-- }
    }

    # publish gaps: own window, render-paced (no iteration >= 50 ms inside), else their row's reason
    $gHit = Get-WindowHits $gaps $wins
    $gRow = Get-RowIndex ([double[]]@($gaps | ForEach-Object { $_.B })) $rows
    $realGaps = New-Object System.Collections.Generic.List[double]
    $gapExcl = @{}
    for ($i = 0; $i -lt $gaps.Count; $i++) {
        $g = $gaps[$i]
        $longest = 0.0
        if ($null -ne $g.N) { for ($k = [long]$g.N - $g.Iters; $k -le [long]$g.N; $k++) { if ($byN.ContainsKey($k) -and $byN[$k] -gt $longest) { $longest = $byN[$k] } } }
        else { foreach ($h in $hitches) { if ($h.A -lt $g.B -and $h.B -gt $g.A -and $h.Wall -gt $longest) { $longest = $h.Wall } } }
        $why = $null; $what = $null
        if ($gHit[$i]) { $why = $gHit[$i].Why; if ($why -in 'harness step', 'agent call') { $what = $gHit[$i].What } }
        elseif ($longest -lt $script:SimMs -and -not ($NoInactiveIdle -and $g.Iters -ge 2)) { $why = 'render-paced' }
        elseif ($gRow[$i] -lt 0) { $why = 'profiler edge' }
        elseif ($rowWhy[$gRow[$i]]) { $why = $rowWhy[$gRow[$i]].Why }
        if (-not $why) { $realGaps.Add($g.Gap); continue }
        Add-Excluded $gapExcl $why $g.Gap
        if ($what -and $g.Gap -ge 300) { Add-Excluded $harness $what 0; $harness[$what].Freezes300++ }
        if ($gRow[$i] -ge 0 -and -not $rowWhy[$gRow[$i]]) { $pubs-- }
    }

    # frames: only their own window counts (frames are not tied to seconds rows)
    $fHit = Get-WindowHits $frames $wins
    $realFrames = New-Object System.Collections.Generic.List[object]
    $frameExcl = @{}
    for ($i = 0; $i -lt $frames.Count; $i++) { if ($fHit[$i]) { Add-Excluded $frameExcl $fHit[$i].Why $frames[$i].Ms } else { $realFrames.Add($frames[$i]) } }

    $realMin = if ($rows.Count) { $keptRows / 60 } else { (($realFrames | Measure-Object Ms -Sum).Sum) / 60000 }
    $invalid = @()
    if ($spanS -lt 0.8 * $Seconds) { $invalid += "profiler cut short ($([math]::Round($spanS)) of $([math]::Round($Seconds)) s)" }
    $busy = @($Machine | Where-Object { $_.t -ge $firstMin })
    if ($busy.Count) {
        $other = ($busy | Measure-Object other -Average).Average
        if ($other -ge 10) { $invalid += ('noisy machine (other programs {0:N1}% of the machine)' -f $other) }
    }
    if ($Crash) { $invalid += $Crash }

    $per10 = [ordered]@{}
    foreach ($t in $script:FreezeThresholds) { $per10["$t"] = if ($realMin -gt 0) { [math]::Round(@($realGaps | Where-Object { $_ -ge $t }).Count * 10 / $realMin, 1) } else { $null } }
    [pscustomobject]@{
        RealMin = [math]::Round($realMin, 2)
        Invalid = $(if ($invalid) { $invalid -join '; ' } else { $null })
        Frames = $(if ($realFrames.Count) { Get-FrameStatsOf @($realFrames | ForEach-Object { $_.Row }) -Total $realFrames.Count -AllFrames:$NoInactiveIdle } else { $null })
        SlowFrames = $(if ($realFrames.Count) { Get-SlowFrames $realFrames } else { $null })
        Sim = $(if ($rows.Count -and $realMin -gt 0) { [pscustomobject]@{ Hitches = $real.Count; HitchesPerMin = [math]::Round($real.Count / $realMin, 2); Iterations = $iters } } else { $null })
        Walls = [double[]]$real.ToArray()
        HasFreezes = [bool]($rows.Count -and (Test-Path -LiteralPath "$Base-freezes.jsonl"))
        Gaps = [double[]]$realGaps.ToArray()
        FreezesPer10 = $per10
        GapP99 = $(if ($pubs -gt 0) { Get-TailPercentile $realGaps.ToArray() $pubs 99 } else { $null })
        GapMax = $(if ($realGaps.Count) { ($realGaps | Measure-Object -Maximum).Maximum } else { 0 })
        PubsPerIter = $(if ($iters -gt 0) { [math]::Round($pubs / $iters, 2) } else { $null })
        Excluded = $excl; GapsExcluded = $gapExcl; FramesExcluded = $frameExcl; Harness = $harness
    }
}

# ------------------------------------------------------------------ pair statistics
# Event counts over real minutes are Poisson: exact (Garwood) 95% interval per variant, Wald interval for the difference
# of rates, exact conditional (Clopper-Pearson) interval for the rate ratio. Below 10 events in both variants together the
# ratio interval spans 1 for every split but the most lopsided ones, so the verdict is "not enough events".
$script:MinEvents = 10
$script:PairThresholds = @(300, 500, 1000, 2000)
$script:Z95 = 1.959964; $script:Z80 = 0.841621

function Get-SumExp([double[]]$L) {
    $m = [double]::NegativeInfinity
    foreach ($x in $L) { if ($x -gt $m) { $m = $x } }
    if ([double]::IsNegativeInfinity($m)) { return 0.0 }
    $s = 0.0; foreach ($x in $L) { $s += [math]::Exp($x - $m) }
    return [math]::Exp($m) * $s
}
function Get-PoissonCdf([int]$K, [double]$Mu) {
    if ($K -lt 0) { return 0.0 }; if ($Mu -le 0) { return 1.0 }
    $l = New-Object double[] ($K + 1); $l[0] = - $Mu
    for ($i = 1; $i -le $K; $i++) { $l[$i] = $l[$i - 1] + [math]::Log($Mu / $i) }
    return [math]::Min(1.0, (Get-SumExp $l))
}
function Get-BinomialCdf([int]$K, [int]$N, [double]$P) {
    if ($K -lt 0) { return 0.0 }; if ($K -ge $N) { return 1.0 }
    $l = New-Object double[] ($K + 1); $l[0] = $N * [math]::Log(1 - $P)
    for ($i = 1; $i -le $K; $i++) { $l[$i] = $l[$i - 1] + [math]::Log(($N - $i + 1) / $i) + [math]::Log($P / (1 - $P)) }
    return [math]::Min(1.0, (Get-SumExp $l))
}
# root of a decreasing $F on [$Lo, $Hi]
function Find-Root([scriptblock]$F, [double]$Lo, [double]$Hi) {
    for ($i = 0; $i -lt 100; $i++) { $m = ($Lo + $Hi) / 2; if ((& $F $m) -gt 0) { $Lo = $m } else { $Hi = $m } }
    return ($Lo + $Hi) / 2
}
function Get-PoissonCI([int]$K) {
    $hi = $K + 10 * [math]::Sqrt($K) + 10
    $lo = if ($K) { Find-Root { param($mu) (Get-PoissonCdf ($K - 1) $mu) - 0.975 } 0 $hi } else { 0.0 }
    return @($lo, (Find-Root { param($mu) (Get-PoissonCdf $K $mu) - 0.025 } 0 $hi))
}
function Get-BinomialCI([int]$K, [int]$N) {
    if (-not $N) { return @(0.0, 1.0) }
    $lo = if ($K) { Find-Root { param($p) (Get-BinomialCdf ($K - 1) $N $p) - 0.975 } 0 1 } else { 0.0 }
    $hi = if ($K -lt $N) { Find-Root { param($p) (Get-BinomialCdf $K $N $p) - 0.025 } 0 1 } else { 1.0 }
    return @($lo, $hi)
}
# events per $Per minutes with the 95% interval
function Get-RateCI([int]$K, [double]$Minutes, [double]$Per = 10) {
    $ci = Get-PoissonCI $K
    $s = if ($Minutes -gt 0) { $Per / $Minutes } else { [double]::NaN }
    [pscustomobject]@{ Est = $K * $s; Lo = $ci[0] * $s; Hi = $ci[1] * $s }
}
# B against A: difference per $Per minutes, ratio B / A, verdict
function Compare-Rates([int]$KA, [double]$MinA, [int]$KB, [double]$MinB, [string]$NameB = 'B', [double]$Per = 10) {
    $n = $KA + $KB; $ra = $KA / $MinA; $rb = $KB / $MinB
    $se = [math]::Sqrt($KB / ($MinB * $MinB) + $KA / ($MinA * $MinA))
    $p = Get-BinomialCI $KB $n
    $odds = { param($x) if ($x -ge 1) { [double]::PositiveInfinity } else { $x / (1 - $x) * $MinA / $MinB } }
    $ratio = [pscustomobject]@{ Est = $(if ($ra) { $rb / $ra } elseif ($n) { [double]::PositiveInfinity } else { [double]::NaN }); Lo = (& $odds $p[0]); Hi = (& $odds $p[1]) }
    $verdict = if ($n -lt $script:MinEvents) { "not enough events ($n < $($script:MinEvents))" }
               elseif ($ratio.Hi -lt 1) { "fewer with $NameB" } elseif ($ratio.Lo -gt 1) { "more with $NameB" } else { 'no clear difference' }
    [pscustomobject]@{ N = $n; Ratio = $ratio; Verdict = $verdict
        Diff = [pscustomobject]@{ Est = ($rb - $ra) * $Per; Lo = ($rb - $ra - $script:Z95 * $se) * $Per; Hi = ($rb - $ra + $script:Z95 * $se) * $Per } }
}
# loads per variant to see a $Reduction cut of a rate of $Per10 events per 10 min at two-sided 5% with 80% power (normal
# approximation on the log rate ratio), $MinPerLoad real minutes per load; $null without events
function Get-LoadsNeeded([double]$Per10, [double]$Reduction, [double]$MinPerLoad) {
    if (-not ($Per10 -gt 0) -or -not ($MinPerLoad -gt 0)) { return $null }
    $l0 = $Per10 / 10; $l1 = $l0 * (1 - $Reduction)
    $mins = [math]::Pow($script:Z95 + $script:Z80, 2) * (1 / $l1 + 1 / $l0) / [math]::Pow([math]::Log(1 - $Reduction), 2)
    return [int][math]::Ceiling($mins / $MinPerLoad)
}

function Get-Mean([object[]]$Values) {
    $v = @($Values | Where-Object { $null -ne $_ })
    if ($v.Count -eq 0) { return $null }
    return [math]::Round(($v | Measure-Object -Average).Average, 2)
}

function Format-Interval($X, [string]$Fmt = 'N1') {
    $c = [Globalization.CultureInfo]::InvariantCulture
    $hi = if ([double]::IsPositiveInfinity($X.Hi)) { 'inf' } else { $X.Hi.ToString($Fmt, $c) }
    return '{0} [{1}, {2}]' -f $X.Est.ToString($Fmt, $c), $X.Lo.ToString($Fmt, $c), $hi
}

# Per variant: the measured loads that are valid; per event kind and threshold the pooled count over the real minutes.
function Format-PairRows($Group, [string[]]$Order) {
    $out = New-Object System.Collections.Generic.List[string]
    $a = $Order[0]; $b = $Order[$Order.Count - 1]
    $arm = @{}
    foreach ($name in $a, $b) { $arm[$name] = @($Group | Where-Object { $_.Variant -eq $name -and $_.M -and -not $_.M.Invalid -and $_.M.RealMin -gt 0 }) }
    $left = @($Group | Where-Object { $_.M -and $_.M.Invalid })
    if ($left) { $out.Add(''); $out.Add('Left out (invalid): ' + (($left | ForEach-Object { "$($_.Name) ($($_.M.Invalid))" }) -join '; ')) }
    if (-not $arm[$a].Count -or -not $arm[$b].Count) { $out.Add(''); $out.Add('No valid measured load in each variant: no pair statistics.'); return $out }
    $out.Add(''); $out.Add("Real hitches only, pooled over each variant's valid loads; $b against $a. Below $($script:MinEvents) events in both together the pair is not read.")
    $out.Add(''); $out.Add("| Events | >= ms | ${a}: n, per 10 min [95% CI] | ${b}: n, per 10 min [95% CI] | $b - $a per 10 min [95% CI] | $b / $a [95% CI] | Verdict | Loads per variant for a 30% cut |")
    $out.Add('|---|---|---|---|---|---|---|---|')
    $minA = ($arm[$a] | ForEach-Object { $_.M.RealMin } | Measure-Object -Sum).Sum
    $minB = ($arm[$b] | ForEach-Object { $_.M.RealMin } | Measure-Object -Sum).Sum
    $kinds = @('iterations')
    if (@($arm[$a] + $arm[$b] | Where-Object { -not $_.M.HasFreezes }).Count -eq 0) { $kinds += 'visible freezes' }
    foreach ($kind in $kinds) {
        foreach ($t in $script:PairThresholds) {
            $count = { param($loads) $n = 0; foreach ($l in $loads) { $v = if ($kind -eq 'iterations') { $l.M.Walls } else { $l.M.Gaps }; $n += @($v | Where-Object { $_ -ge $t }).Count }; $n }
            $ka = & $count $arm[$a]; $kb = & $count $arm[$b]
            $c = Compare-Rates $ka $minA $kb $minB $b
            $ra = Get-RateCI $ka $minA
            $need = Get-LoadsNeeded $ra.Est 0.3 ($minA / $arm[$a].Count)
            $out.Add(('| {0} | {1} | {2}, {3} | {4}, {5} | {6} | {7} | {8} | {9} |' -f $kind, $t, $ka, (Format-Interval $ra), $kb, (Format-Interval (Get-RateCI $kb $minB)),
                $(if ($c.N) { Format-Interval $c.Diff } else { '-' }), $(if ($c.N) { Format-Interval $c.Ratio 'N2' } else { '-' }), $c.Verdict,
                $(if ($null -ne $need) { $need } else { "no $a events" })))
        }
    }
    return $out
}

function Format-ExcludedRows([hashtable]$Table, [string]$Unit) {
    $out = New-Object System.Collections.Generic.List[string]
    if (-not $Table) { return $out }
    $keys = @($script:ExclusionReasons | Where-Object { $Table.ContainsKey($_) })
    if (-not $keys) { return $out }
    $out.Add(''); $out.Add("| Excluded $Unit | n | ms | >= 300 | >= 1000 | max ms | min dropped |"); $out.Add('|---|---|---|---|---|---|---|')
    foreach ($k in $keys) { $r = $Table[$k]; $out.Add(('| {0} | {1} | {2:N0} | {3} | {4} | {5:N0} | {6:N1} |' -f $k, $r.N, $r.Ms, $r.N300, $r.N1000, $r.Max, ($r.Secs / 60))) }
    return $out
}

# One section per measured load: what was left out and why, the harness commands' own cost, and slow frames by stage.
function Format-LoadDetail($R) {
    $m = $R.M
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add(''); $out.Add("### $($R.Name)")
    $out.Add('')
    $out.Add(('Real minutes {0}{1}. Visible freezes (publish gaps) per 10 min >= 100 / 300 / 500 / 1000 / 2000 ms: {2}; gap p99 {3} ms, max {4:N0} ms; publishes per iteration {5}.' -f
        $m.RealMin, $(if ($m.Invalid) { "; INVALID: $($m.Invalid)" } else { '' }),
        $(if ($m.HasFreezes) { ($m.FreezesPer10.Values | ForEach-Object { if ($null -ne $_) { $_ } else { '-' } }) -join ' / ' } else { 'not recorded' }),
        $(if ($null -ne $m.GapP99) { [math]::Round($m.GapP99) } elseif ($m.HasFreezes) { '< 50' } else { '-' }), $m.GapMax, $(if ($null -ne $m.PubsPerIter) { $m.PubsPerIter } else { '-' })))
    foreach ($l in (Format-ExcludedRows $m.Excluded 'iterations (>= 50 ms) and seconds')) { $out.Add($l) }
    foreach ($l in (Format-ExcludedRows $m.GapsExcluded 'publish gaps (>= 50 ms)')) { $out.Add($l) }
    foreach ($l in (Format-ExcludedRows $m.FramesExcluded 'frames')) { $out.Add($l) }
    if ($m.Harness.Count) {
        $out.Add(''); $out.Add('Harness commands: their own game cost, not in the rates above.')
        $out.Add(''); $out.Add('| Command | iterations >= 50 ms | >= 300 | >= 1000 | max ms | freezes >= 300 |'); $out.Add('|---|---|---|---|---|---|')
        foreach ($k in ($m.Harness.Keys | Sort-Object)) { $h = $m.Harness[$k]; $out.Add(('| {0} | {1} | {2} | {3} | {4:N0} | {5} |' -f $k, $h.N, $h.N300, $h.N1000, $h.Max, $h.Freezes300)) }
    }
    $s = $m.SlowFrames
    if ($s -and $s.N) {
        $out.Add(''); $out.Add("Frames >= $($s.Ms) ms: $($s.N). By main-loop stage (lead = the frame's longest stage):")
        $out.Add(''); $out.Add('| Stage | lead | ms |'); $out.Add('|---|---|---|')
        foreach ($x in $s.Stages) { $out.Add("| $($x.Stage) | $($x.Lead) | $($x.Ms) |") }
        if ($s.Services) {
            $out.Add(''); $out.Add('| Service (in those frames) | lead | ms |'); $out.Add('|---|---|---|')
            foreach ($x in $s.Services) { $out.Add("| $($x.Service) | $($x.Lead) | $($x.Ms) |") }
        }
    }
    return $out
}

# results.md: one row per load, per measured load its detail, then per job the mean of each variant over its loads and
# the pair statistics.
function Format-Results($Results, [string]$Title) {
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add("# $Title"); $out.Add('')
    $out.Add('| Load | Result | Closed | Real min | Fps | p50 ms | p95 ms | p99 ms | max ms | >50 ms/min | Sim hitches/min | Freezes >= 300 ms /10 min | Notes |')
    $out.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($r in $Results) {
        $f = $r.Frames; $s = $r.Sim; $m = $r.M
        $notes = @($r.Notes | ForEach-Object { $_ -replace '\|', '/' })
        if ($r.PSObject.Properties['Events'] -and $r.Events) { $notes = @("repro events: $($r.Events)") + $notes }
        if ($m -and $m.Invalid) { $notes = @("INVALID: $($m.Invalid)") + $notes }
        $cells = @($r.Name, $(if ($r.Ok) { 'pass' } else { 'FAIL' }), $r.Closed, $(if ($m) { $m.RealMin } else { '' }))
        if ($f -and $f.Frames -gt 0) { $cells += @($f.Fps, $f.P50, $f.P95, $f.P99, $f.Max, $f.Over50PerMin) } else { $cells += @('', '', '', '', '', '') }
        $cells += @($(if ($s) { $s.HitchesPerMin } else { '' }), $(if ($m -and $m.HasFreezes) { $m.FreezesPer10['300'] } else { '' }), ($notes -join '; '))
        $out.Add('| ' + ($cells -join ' | ') + ' |')
    }
    foreach ($r in @($Results | Where-Object { $_.M })) { foreach ($l in (Format-LoadDetail $r)) { $out.Add($l) } }
    foreach ($group in ($Results | Group-Object { $_.JobId })) {
        $variants = @($group.Group | Group-Object Variant)
        if ($variants.Count -lt 2) { continue }
        $order = @($group.Group | ForEach-Object { $_.Variant } | Select-Object -Unique)
        $out.Add(''); $out.Add("## $($group.Name): A/B")
        $out.Add(''); $out.Add('Order: ' + (($group.Group | ForEach-Object { $_.Variant }) -join ', '))
        $out.Add(''); $out.Add('| Variant | Loads | Passed | p50 ms | p99 ms | >50 ms/min | Sim hitches/min |')
        $out.Add('|---|---|---|---|---|---|---|')
        foreach ($v in $variants) {
            $g = @($v.Group | Where-Object { -not ($_.M -and $_.M.Invalid) })
            $out.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f $v.Name, $g.Count, @($g | Where-Object Ok).Count,
                (Get-Mean @($g | ForEach-Object { if ($_.Frames) { $_.Frames.P50 } })),
                (Get-Mean @($g | ForEach-Object { if ($_.Frames) { $_.Frames.P99 } })),
                (Get-Mean @($g | ForEach-Object { if ($_.Frames) { $_.Frames.Over50PerMin } })),
                (Get-Mean @($g | ForEach-Object { if ($_.Sim) { $_.Sim.HitchesPerMin } }))))
        }
        foreach ($l in (Format-PairRows $group.Group $order)) { $out.Add($l) }
    }
    $out.Add('')
    $out.Add('Only real hitches count: the first minute, the profiler edges, harness commands and render-paced gaps are')
    $out.Add('left out and listed per load, and invalid loads (cut short, noisy machine, crash) leave the means and pairs.')
    $out.Add('Loads are separate game starts; compare a difference with the spread between loads of the same variant.')
    return ($out -join "`r`n") + "`r`n"
}

# Progress lines go to the host, not the output: a function's output is its return value, and a status line written
# to it lands in the caller's results (the first live run had 46 result rows for one load).
function Say([string]$Text) { Write-Host ('{0:HH:mm:ss} {1}' -f (Get-Date), $Text) }

# What the wait for the test agent sees now: the game process, Apex's log of this start, the agent's marker file in
# the user folder ($DocsDir) and, once found, its mailbox (any object with Found and Heartbeat).
function Get-StartObservation($Proc, [string]$LogPath, [string]$DocsDir, [datetime]$Started, $Mbox) {
    $o = [pscustomobject]@{ Sec = ((Get-Date) - $Started).TotalSeconds; Alive = -not $Proc.HasExited; ExitCode = $null; Cpu = 0.0
                            LogBytes = -1; Features = $false; Marker = $null; Found = $false; Beat = $null }
    if ($o.Alive) { $Proc.Refresh(); $o.Cpu = $Proc.TotalProcessorTime.TotalSeconds } else { $o.ExitCode = $Proc.ExitCode }
    $log = Get-Item -LiteralPath $LogPath -ErrorAction SilentlyContinue
    if ($log -and $log.LastWriteTime -ge $Started) { # Apex starts a new log at every game start
        $o.LogBytes = $log.Length
        $o.Features = [bool](Select-String -LiteralPath $LogPath -SimpleMatch '[Main] Features started' -List -ErrorAction SilentlyContinue)
    }
    foreach ($f in @(Get-ChildItem -LiteralPath $DocsDir -Filter 'ScriptError*.xml' -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $Started })) {
        $line = @(Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue) -match '<ApexTestAgent ' | Select-Object -First 1
        if ($line) { $o.Marker = $line.Trim(); break }
    }
    if ($Mbox -and $Mbox.Found) { $o.Found = $true; try { $o.Beat = $Mbox.Heartbeat } catch { } }
    return $o
}

# One 30 s check of the wait for the test agent: the reason to stop, or $null to keep waiting. $State carries the last
# check. Rules: the game runs; Apex's log has its first lines 60 s after the start; CPU time, the log or the agent's
# heartbeat moves (two flat checks in a row fail); $AgentSec after Apex started its features (the main menu follows
# within seconds) the agent's mailbox is found, and the message tells "not loaded" (no marker) from "loaded but its
# mailbox not found" (marker).
function Step-StartWatch([hashtable]$State, $Obs, [int]$AgentSec = 90) {
    if (-not $Obs.Alive) { return "the game closed before the test agent answered (exit code $($Obs.ExitCode))" }
    if ($Obs.LogBytes -lt 0 -and $Obs.Sec -ge 60) {
        return 'no ApexRadiance_LOG.txt from this start after 60 s: ApexRadiance.asi did not load (is an ASI loader in Game\Bin? see -AsiLoader)'
    }
    $moved = (-not $State.ContainsKey('Cpu')) -or ($Obs.Cpu - $State.Cpu -gt 1) -or ($Obs.LogBytes -ne $State.LogBytes) -or ($Obs.Beat -ne $State.Beat)
    $State.Misses = if ($moved) { 0 } else { $State.Misses + 1 }
    $State.Cpu = $Obs.Cpu; $State.LogBytes = $Obs.LogBytes; $State.Beat = $Obs.Beat
    if ($State.Misses -ge 2) { return 'no progress for 60 s: CPU time, ApexRadiance_LOG.txt and the agent heartbeat did not move (the game hangs)' }
    if ($Obs.Features -and -not $State.ContainsKey('FeaturesAt')) { $State.FeaturesAt = $Obs.Sec }
    if ($State.ContainsKey('FeaturesAt') -and -not $Obs.Found -and $Obs.Sec - $State.FeaturesAt -ge $AgentSec) {
        if (-not $Obs.Marker) {
            return "the test agent did not load: no ApexTestAgent marker (ScriptError_*.xml) and no mailbox $AgentSec s after Apex started its features (check Mods\Resource.cfg, Mods\Packages\ApexTestAgent.package and the game's script errors)"
        }
        return "the test agent loaded ($($Obs.Marker)) but its mailbox was not found in the game's memory"
    }
    return $null
}

# 0: every planned load ran and passed; 1: a load failed; 2: the run stopped before every load ran.
function Get-RunExitCode($Results, [int]$Planned) {
    $rows = @($Results | Where-Object { $_ })
    if ($rows.Count -lt $Planned) { return 2 }
    if (@($rows | Where-Object { -not $_.Ok }).Count) { return 1 }
    return 0
}

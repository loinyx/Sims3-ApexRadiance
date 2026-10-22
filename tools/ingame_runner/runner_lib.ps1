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

# Frame statistics from the frame-times.js CSV (epochMs,qpcMs,frameMs,workMs,fg). Only frames with the game window in
# front count: the game caps itself at about 30 FPS in the background.
function Get-FrameStats([string]$CsvPath) {
    if (-not (Test-Path -LiteralPath $CsvPath)) { return $null }
    $rows = @(Import-Csv -LiteralPath $CsvPath)
    if ($rows.Count -lt 2) { return $null }
    $fg = @($rows | Where-Object { $_.fg -eq '1' })
    $ms = [double[]]@($fg | ForEach-Object { [double]::Parse($_.frameMs, [Globalization.CultureInfo]::InvariantCulture) } | Sort-Object)
    if ($ms.Count -eq 0) { return [pscustomobject]@{ Frames = 0; FocusedShare = 0 } }
    $totalMin = ($ms | Measure-Object -Sum).Sum / 60000
    [pscustomobject]@{
        Frames = $ms.Count
        FocusedShare = [math]::Round($fg.Count / $rows.Count, 3)
        Fps = [math]::Round($ms.Count / ($totalMin * 60), 1)
        P50 = [math]::Round((Get-Percentile $ms 50), 2)
        P95 = [math]::Round((Get-Percentile $ms 95), 2)
        P99 = [math]::Round((Get-Percentile $ms 99), 2)
        Max = [math]::Round($ms[$ms.Count - 1], 2)
        Over50PerMin = [math]::Round(@($ms | Where-Object { $_ -gt 50 }).Count / $totalMin, 2)
    }
}

# Simulation-thread hitches from stutter-profile.js (<base>-hitches.jsonl: one line per iteration over its threshold).
function Get-SimStats([string]$Base, [double]$Minutes) {
    $hitches = "$Base-hitches.jsonl"
    if (-not (Test-Path -LiteralPath $hitches) -or $Minutes -le 0) { return $null }
    $n = @(Get-Content -LiteralPath $hitches | Where-Object { $_.Trim() }).Count
    [pscustomobject]@{ Hitches = $n; HitchesPerMin = [math]::Round($n / $Minutes, 2) }
}

function Get-Mean([object[]]$Values) {
    $v = @($Values | Where-Object { $null -ne $_ })
    if ($v.Count -eq 0) { return $null }
    return [math]::Round(($v | Measure-Object -Average).Average, 2)
}

# results.md: one row per load, then per job the mean of each variant over its loads.
function Format-Results($Results, [string]$Title) {
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add("# $Title"); $out.Add('')
    $out.Add('| Load | Result | Closed | Fps | p50 ms | p95 ms | p99 ms | max ms | >50 ms/min | Sim hitches/min | Notes |')
    $out.Add('|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($r in $Results) {
        $f = $r.Frames; $s = $r.Sim
        $cells = @($r.Name, $(if ($r.Ok) { 'pass' } else { 'FAIL' }), $r.Closed)
        if ($f -and $f.Frames -gt 0) { $cells += @($f.Fps, $f.P50, $f.P95, $f.P99, $f.Max, $f.Over50PerMin) } else { $cells += @('', '', '', '', '', '') }
        $cells += @($(if ($s) { $s.HitchesPerMin } else { '' }), (($r.Notes | ForEach-Object { $_ -replace '\|', '/' }) -join '; '))
        $out.Add('| ' + ($cells -join ' | ') + ' |')
    }
    foreach ($group in ($Results | Group-Object { $_.JobId })) {
        $variants = @($group.Group | Group-Object Variant)
        if ($variants.Count -lt 2) { continue }
        $out.Add(''); $out.Add("## $($group.Name): A/B")
        $out.Add(''); $out.Add('Order: ' + (($group.Group | ForEach-Object { $_.Variant }) -join ', '))
        $out.Add(''); $out.Add('| Variant | Loads | Passed | p50 ms | p99 ms | >50 ms/min | Sim hitches/min |')
        $out.Add('|---|---|---|---|---|---|---|')
        foreach ($v in $variants) {
            $g = $v.Group
            $out.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f $v.Name, $g.Count, @($g | Where-Object Ok).Count,
                (Get-Mean @($g | ForEach-Object { if ($_.Frames) { $_.Frames.P50 } })),
                (Get-Mean @($g | ForEach-Object { if ($_.Frames) { $_.Frames.P99 } })),
                (Get-Mean @($g | ForEach-Object { if ($_.Frames) { $_.Frames.Over50PerMin } })),
                (Get-Mean @($g | ForEach-Object { if ($_.Sim) { $_.Sim.HitchesPerMin } }))))
        }
    }
    $out.Add('')
    $out.Add('Frame times count only frames with the game window in front. Loads are separate game starts; compare a')
    $out.Add('difference with the spread between loads of the same variant before reading it as an effect.')
    return ($out -join "`r`n") + "`r`n"
}

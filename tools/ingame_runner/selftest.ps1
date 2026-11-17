# Offline checks of the in-game runner: no game, nothing installed, scratch files under %TEMP% only.
# Covers the TOML switches, the ABBA load order, the log pass rule, the frame statistics, one result row per load, the
# exit codes, the start-up watch on a stand-in game, log and agent marker, every job file, and the mailbox client
# against an in-process stand-in for the game's agent (same layout, same magic).
#   ./tools/ingame_runner/selftest.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runner_lib.ps1')
$script:checks = 0; $script:failures = 0
function Check([bool]$Ok, [string]$What) { $script:checks++; if (-not $Ok) { $script:failures++; Write-Output "FAIL: $What" } }

# TOML: replace a key, add a key to an existing table, add a table, leave other tables alone
$t = "[ui]`r`ndeveloper_mode = false`r`n`r`n[patches.FastMemory]`r`nenabled = true`r`nspin = 4`r`n`r`n[patches.Other]`r`nenabled = true`r`n"
$r = Set-TomlValue $t 'patches.FastMemory' 'enabled' $false
Check ($r -match '\[patches\.FastMemory\]\r\nenabled = false\r\nspin = 4') 'existing key replaced in place'
Check ($r -match '\[patches\.Other\]\r\nenabled = true') 'other table unchanged'
$r = Set-TomlValue $t 'patches.Other' 'budget_ms' 3.5
Check ($r -match '\[patches\.Other\]\r\nbudget_ms = 3\.5\r\nenabled = true') 'missing key added under its table'
$r = Set-TomlValue $t 'patches.ResourceLookupCache' 'enabled' $true
Check ($r.TrimEnd().EndsWith("[patches.ResourceLookupCache]`r`nenabled = true")) 'missing table appended'
Check ((Set-TomlValue '' 'ui' 'developer_mode' $true) -eq "[ui]`r`ndeveloper_mode = true`r`n") 'empty file'
Check ((ConvertTo-TomlLiteral 'a"b\c') -eq '"a\"b\\c"') 'string escaping'
$o = '{ "patches.A": { "enabled": false }, "ui": { "developer_mode": true } }' | ConvertFrom-Json
$r = Set-TomlOverrides $t $o
Check ($r -match '\[patches\.A\]\r\nenabled = false' -and $r -match 'developer_mode = true') 'overrides from job JSON'
$t2 = "[ patches.FastMemory ]  # tuned`r`nenabled = true`r`n[patches.Other] # x`r`nenabled = true`r`n"
$r = Set-TomlValue (Set-TomlValue $t2 'patches.FastMemory' 'enabled' $false) 'patches.Other' 'enabled' $false
Check ($r -match '\[ patches\.FastMemory \]  # tuned\r\nenabled = false' -and $r -match '\[patches\.Other\] # x\r\nenabled = false' -and ([regex]::Matches($r, 'patches\.')).Count -eq 2) 'header with spaces or a comment found, no duplicate table'

# Load order
$job = '{ "id": "x", "variants": { "off": {}, "on": {} } }' | ConvertFrom-Json
Check ((@(Get-JobLoads $job -AB | ForEach-Object Variant) -join ',') -eq 'off,on,on,off') 'ABBA order'
Check ((@(Get-JobLoads $job | ForEach-Object Variant) -join ',') -eq 'on') 'default is the last variant'
Check ((@(Get-JobLoads $job -Variant off | ForEach-Object Name) -join ',') -eq 'x-1-off') 'one variant'
$failed = $false; try { Get-JobLoads $job -Variant nope | Out-Null } catch { $failed = $true }
Check $failed 'unknown variant refused'

# Log pass rule
$log = @('12:00:00.000 [INFO] [Patches] 21 features: A, B', '12:00:01.000 [INFO] [ResourceCache] On: FindProvider 0x004AFFC0',
         '12:00:02.000 [ERROR] [ObjectIndex] Verification mismatch: manager 1; the index turns itself off for this session')
$c = Test-ApexLog $log @('\[ResourceCache\] On:') $script:DefaultFailPatterns
Check (-not $c.Ok -and $c.Failures.Count -eq 1 -and $c.Missing.Count -eq 0) 'mismatch fails the load'
$c = Test-ApexLog $log[0..1] @('\[ResourceCache\] On:', '\[FileListCache\] On:') $script:DefaultFailPatterns
Check (-not $c.Ok -and $c.Missing -contains '\[FileListCache\] On:') 'missing expected line fails the load'
Check (Test-ApexLog $log[0..1] @('\[ResourceCache\] On:') $script:DefaultFailPatterns).Ok 'clean log passes'
foreach ($bad in '12:00:03.000 [WARNING] [SceneBudget] Node 0x0a0b0c0d was found destroyed but still queued in a drain; unlinked and skipped (please report)',
                 '12:00:03.000 [WARNING] [LotLightingMotion] Lot build slice left as it is (the code at 0x00123456 differs or could not be written)',
                 '12:00:03.000 [WARNING] [RoomLightQueue] Could not put back the game code at 0x123456',
                 '12:00:03.000 [INFO] [RoomLightQueue] Stopped, but some game code could not be put back') {
    Check (-not (Test-ApexLog @($bad) @() $script:DefaultFailPatterns).Ok) "fails a load: $($bad.Substring(13, 40))"
}

# Real-time GI start line and profiler-arm patterns, taken from the shipped jobs
$giStart = @((Get-Content -LiteralPath (Join-Path $PSScriptRoot 'jobs/realtime-gi-smoke.json') -Raw | ConvertFrom-Json).variants.on.expect)[0]
foreach ($ok in '12:00:00.000 [INFO] [RealtimeGI] On: lots=0 cap=class0 scale=1 indirect=off backend=d3d9',
                '12:00:00.000 [INFO] [RealtimeGI] On: lots=1 cap=class0 scale=1 indirect=off backend=d3d9',
                '12:00:00.000 [INFO] [RealtimeGI] On: lots=3 cap=hold scale=2 indirect=atlas backend=serverRays') {
    Check ($ok -match $giStart) "GI start line matches: $($ok.Substring(13))"
}
foreach ($bad in '12:00:00.000 [INFO] [RealtimeGI] Off: lots=1 cap=class0 scale=1 indirect=off backend=d3d9',
                 '12:00:00.000 [INFO] [RealtimeGI] On: lots=1 cap=class0 scale=1 indirect=off',
                 '12:00:00.000 [INFO] [RealtimeGI] On: lots=x cap=class0 scale=1 indirect=off backend=d3d9',
                 '12:00:00.000 [INFO] [RealtimeGI] On: lots=1 cap=class3 scale=1 indirect=off backend=d3d9',
                 '12:00:00.000 [INFO] [RealtimeGI] On: lots=1 cap=class0 scale=3 indirect=off backend=d3d9',
                 '12:00:00.000 [INFO] [RealtimeGIX] On: lots=1 cap=class0 scale=1 indirect=off backend=d3d9',
                 '12:00:00.000 [INFO] [RtOutdoorLamps] On: lots=1 cap=class0 scale=1 indirect=off backend=d3d9') {
    Check ($bad -notmatch $giStart) "GI start line rejected: $($bad.Substring(13))"
}
# The GI jobs set only keys the module registers (features/realtime_gi.cpp), so a job never relies on a key that is ignored
$giSrc = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../../features/realtime_gi.cpp') -Raw
$giKeys = @('enabled') + @([regex]::Matches($giSrc, 'Register\w*Setting\(&\w+, "(\w+)"') | ForEach-Object { $_.Groups[1].Value })
foreach ($job in @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'jobs') -Filter '*.json' | Where-Object { $_.Name -like 'realtime-gi-*' -or $_.Name -like 'gi-*' })) {
    $j = Get-Content -LiteralPath $job.FullName -Raw | ConvertFrom-Json
    foreach ($v in @($j.variants.PSObject.Properties)) {
        $t = $v.Value.toml.'patches.RealtimeGI'
        if ($t) { foreach ($k in $t.PSObject.Properties.Name) { Check ($k -in $giKeys) "$($job.BaseName) $($v.Name): RealtimeGI key $k is registered" } }
    }
}
# Lamp index counter lines (lamp-index-smoke): objects, fences and windows, the line RealtimeGI OnPresent writes
$li = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'jobs/lamp-index-smoke.json') -Raw | ConvertFrom-Json
$liCompare = @($li.variants.compare.expect)[1]; $liDraw = @($li.variants.draw.expect)[1]; $liBad = @($li.variants.compare.fail)[0]
$liLine = '12:00:00.000 [INFO] [RealtimeGI] Lamp index counters: mode={0} builds=3 cells=12288 draws={1} fallbacks=40 compared={2} mismatches: offCentre=10 height=4 cellFull=0 group={3} unexplained={4} fences: draws={5} compared={6} windows: draws=0 compared=0'
Check (($liLine -f 'compare', 0, 900, 50, 0, 0, 120) -match $liCompare) 'lamp index compare line matches (fences compared, windows 0)'
Check (($liLine -f 'compare', 0, 900, 50, 0, 0, 0) -notmatch $liCompare) 'lamp index compare line rejected without fence compares'
Check (($liLine -f 'compare', 0, 900, 50, 2, 0, 120) -notmatch $liCompare) 'lamp index compare line rejected with unexplained differences'
Check (($liLine -f 'compare', 0, 900, 50, 2, 0, 120) -match $liBad) 'lamp index fail matches unexplained differences'
Check (($liLine -f 'compare', 0, 900, 50, 0, 0, 120) -notmatch $liBad) 'lamp index fail ignores unexplained=0'
Check (($liLine -f 'draw', 700, 0, 0, 0, 300, 0) -match $liDraw) 'lamp index draw line matches (fence draws)'
Check (($liLine -f 'draw', 700, 0, 0, 0, 0, 0) -notmatch $liDraw) 'lamp index draw line rejected without fence draws'
Check (($liLine -f 'draw', 0, 0, 0, 0, 300, 0) -notmatch $liDraw) 'lamp index draw line rejected without draws'
# The indirect light smoke (S2-05): its counters pattern takes a line with ready texels and draws, not one without
$giInd = @((Get-Content -LiteralPath (Join-Path $PSScriptRoot 'jobs/gi-indirect-smoke.json') -Raw | ConvertFrom-Json).variants.on.expect)[1]
$giLine = '12:00:10.000 [INFO] [RealtimeGI] Counters: frames=600 backend=serverRays draws=4 deviceCalls=2 lamps=12 lots=1 lampChanges=0 indirect=atlas atlasReady={0} gridTexels=900 indirectDraws={1} wallDraws=12 rigs=40 rigDraws={2} rigReads=2 rigSkipped=0; must be 0: capSolves=0 syncReadbacks=0 locks=0 redrawDiffs=0 readFaults=0 lampTableDiffs=0'
Check (($giLine -f 640, 37, 300) -match $giInd) 'GI indirect counters match with ready texels and draws'
foreach ($bad in @(@(0, 37, 300), @(640, 0, 300), @(640, 37, 0))) { Check (($giLine -f $bad[0], $bad[1], $bad[2]) -notmatch $giInd) "GI indirect counters rejected: atlasReady=$($bad[0]) indirectDraws=$($bad[1]) rigDraws=$($bad[2])" }
# The G-buffer bridge view (WP11): the counter line with views and no fault, not one with a fault; the off line fails
$gbJob = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'jobs/gbuffer-motion-smoke.json') -Raw | ConvertFrom-Json
$gbView = @($gbJob.variants.bridge.expect)[2]; $gbOff = @($gbJob.fail)[-1]
Check ('12:00:10.000 [INFO] [GBuffer] Bridge view: views=600 viewFaults=0' -match $gbView) 'bridge view counters match with views and no fault'
foreach ($bad in 'views=0 viewFaults=0', 'views=600 viewFaults=2') { Check ("12:00:10.000 [INFO] [GBuffer] Bridge view: $bad" -notmatch $gbView) "bridge view counters rejected: $bad" }
Check ('12:00:00.000 [INFO] [GBuffer] Bridge view off: no bridge export (needs the bridge pair of patch 0024)' -match $gbOff) 'bridge view off line fails the load'
Check ('12:00:00.000 [INFO] [GBuffer] Bridge view on' -notmatch $gbOff) 'bridge view on line does not fail the load'
$prof = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'jobs/realtime-gi-profile.json') -Raw | ConvertFrom-Json
Check ('12:00:00.000 [INFO] [FrameProfiler] Measurement preset: timing run (hitch 2.0 x median, floor 8 ms)' -match @($prof.expect)[0]) 'profile arm expect matches the preset line'
Check ('12:00:00.000 [INFO] [FrameProfiler] Measurement preset: sampling run (hitch)' -notmatch @($prof.expect)[0]) 'profile arm expect rejects the sampling preset'
Check ('12:00:00.000 [WARNING] [FrameProfiler] Room solve: rebuild: not found' -match @($prof.fail)[0]) 'profile arm fail matches a missing counter'
Check ('12:00:00.000 [INFO] [FrameProfiler] Lot room solve: Timed' -notmatch @($prof.fail)[0]) 'profile arm fail ignores the Lot room solve line'

# Frame statistics: 100 focused frames of 16 ms, one of 120 ms, 10 unfocused frames that do not count
$csv = Join-Path ([IO.Path]::GetTempPath()) ('apex-ingame-selftest-' + [Guid]::NewGuid().ToString('N') + '.csv')
$rows = @('epochMs,qpcMs,frameMs,workMs,fg') + (1..100 | ForEach-Object { "$_,$_,16,5,1" }) + @('101,101,120,5,1') + (1..10 | ForEach-Object { "$_,$_,33,5,0" })
try {
    Set-Content -LiteralPath $csv -Value $rows
    $f = Get-FrameStats $csv
    Check ($f.Frames -eq 101 -and $f.P50 -eq 16 -and $f.Max -eq 120) 'frame percentiles over focused frames'
    Check ([math]::Abs($f.Over50PerMin - (1 / (1720 / 60000))) -lt 0.1) 'long frames per minute'
} finally { Remove-Item -LiteralPath $csv -ErrorAction SilentlyContinue }

# Pair statistics: exact Poisson (Garwood) and Clopper-Pearson intervals, verdict rule, loads needed
$ci = Get-PoissonCI 0; Check ($ci[0] -eq 0 -and [math]::Abs($ci[1] - 3.689) -lt 0.001) 'Garwood k=0'
$ci = Get-PoissonCI 10; Check ([math]::Abs($ci[0] - 4.795) -lt 0.001 -and [math]::Abs($ci[1] - 18.39) -lt 0.01) 'Garwood k=10'
$ci = Get-PoissonCI 2000; Check ($ci[0] -gt 1900 -and $ci[1] -lt 2100) 'Garwood k=2000 without overflow'
$ci = Get-BinomialCI 5 10; Check ([math]::Abs($ci[0] - 0.187) -lt 0.001 -and [math]::Abs($ci[1] - 0.813) -lt 0.001) 'Clopper-Pearson 5/10'
Check ((Compare-Rates 20 100 2 100 'on').Verdict -eq 'fewer with on') '2 vs 20 events: fewer'
Check ((Compare-Rates 4 100 3 100 'on').Verdict -eq 'not enough events (7 < 10)') '3 vs 4 events: not enough'
Check ((Compare-Rates 33 100 30 100 'on').Verdict -eq 'no clear difference') '30 vs 33 events: no clear difference'
Check ((Compare-Rates 10 100 40 100 'on').Verdict -eq 'more with on') '40 vs 10 events: more'
$c = Compare-Rates 10 100 40 100 'on'; Check ([math]::Abs($c.Diff.Est - 3) -lt 1e-9 -and $c.Ratio.Est -eq 4 -and $c.Ratio.Lo -gt 1) 'difference and ratio'
Check ((Get-LoadsNeeded 5.4 0.3 3.5) -eq 80 -and $null -eq (Get-LoadsNeeded 0 0.3 3.5)) 'loads needed for a 30% cut'
Check ((Get-TailPercentile @(60, 80, 400) 1000 99) -eq $null -and (Get-TailPercentile @(60, 80, 400) 1000 99.9) -eq 400) 'percentile from the tail'

# Real hitches only: a fixture profile of 120 s with every exclusion reason
$fx = Join-Path ([IO.Path]::GetTempPath()) ('apex-ingame-selftest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fx | Out-Null
try {
    $base = Join-Path $fx 'measure3'
    Set-Content -LiteralPath "$base-seconds.csv" -Value (@('iters,maxIterMs,publishes,t') + (1..120 | ForEach-Object { "30,40,30,$($_ * 1000)" }))
    $h = @(@{ t = 30000; wallMs = 400; steps = 1; n = 50 },     # first minute
           @{ t = 70000; wallMs = 350; steps = 1; n = 100 },    # real
           @{ t = 80000; wallMs = 500; steps = 1; n = 120 },    # inside the camera_pan step
           @{ t = 95000; wallMs = 1200; steps = 0; n = 150 },   # save or load
           @{ t = 100000; wallMs = 300; steps = 1; n = 160 },   # agent call
           @{ t = 110000; wallMs = 60; steps = 1; n = 170 },    # real
           @{ t = 119500; wallMs = 200; steps = 1; n = 180 })   # profiler edge
    Set-Content -LiteralPath "$base-hitches.jsonl" -Value ($h | ForEach-Object { $_ | ConvertTo-Json -Compress })
    $g = @(@{ t0 = 69600; t1 = 70050; gapMs = 450; prevGapMs = 30; n = 100; iters = 1 },   # the 350 ms iteration inside: real
           @{ t0 = 105000; t1 = 105400; gapMs = 400; prevGapMs = 30; n = 200; iters = 3 }, # no slow iteration, 3 iterations
           @{ t0 = 106000; t1 = 106200; gapMs = 200; prevGapMs = 30; n = 210; iters = 1 }) # no slow iteration: render-paced
    Set-Content -LiteralPath "$base-freezes.jsonl" -Value ($g | ForEach-Object { $_ | ConvertTo-Json -Compress })
    $head = 'epochMs,qpcMs,frameMs,workMs,fg,loadMs,idleMs,svcMs,consumeMs,captureMs,rcbMs,beginMs,drawMs,presentMs,postMs,endMs,otherMs,consumes,tryFail,notReady,drains,msgs,svc1,svc1Ms,svc2,svc2Ms,svc3,svc3Ms'
    # one slow frame at 70092 ms led by the services stage (Canvas 60 of its 80 ms)
    $fr = @($head) + (1..3600 | ForEach-Object { $q = $_ * 33
        if ($q -eq 70092) { "$q,$q,120,5,0,0,na,90,10,3,1,1,20,2,1,1,1,1,0,0,1,4,Canvas,60,UTFWin,20,," } else { "$q,$q,33,5,0,0,na,5,1,3,1,1,20,2,1,1,1,1,0,0,1,4,Canvas,2,,,," } })
    Set-Content -LiteralPath "$base-frames.csv" -Value $fr
    # the same collection types run.ps1 passes (List[object])
    $cmds = New-Object System.Collections.Generic.List[object]
    $cmds.Add([pscustomobject]@{ label = 'camera_pan'; kind = 'step'; t0 = 79000; t1 = 79500 }); $cmds.Add([pscustomobject]@{ label = 'game_state'; kind = 'call'; t0 = 99900; t1 = 99950 })
    $quiet = New-Object System.Collections.Generic.List[object]
    $quiet.Add([pscustomobject]@{ t = 1000; game = 30; other = 50 }); $quiet.Add([pscustomobject]@{ t = 90000; game = 30; other = 2 })
    $m = Get-Measurement $base 120 $cmds $quiet
    Check ($m.RealMin -eq 0.75) "real minutes leave excluded rows (got $($m.RealMin))"
    Check (($m.Walls -join ',') -eq '350,60' -and $m.Sim.Hitches -eq 2) 'only real iterations count'
    Check ($m.Excluded['first minute'].N -eq 1 -and $m.Excluded['profiler edge'].N -eq 1 -and $m.Excluded['harness step'].N -eq 2 -and $m.Excluded['agent call'].N -eq 1) 'iterations excluded by reason'
    Check ($m.Harness['camera_pan'].N -eq 1 -and $m.Harness['save or load (iteration without a sim step)'].N -eq 1 -and $m.Harness['game_state'].N -eq 1) 'harness commands reported as their own cost'
    Check (($m.Gaps -join ',') -eq '450' -and $m.GapsExcluded['render-paced'].N -eq 2) 'publish gaps without a slow iteration are render-paced'
    $m2 = Get-Measurement $base 120 $cmds $quiet -NoInactiveIdle
    Check (($m2.Gaps -join ',') -eq '450,400' -and $m2.GapsExcluded['render-paced'].N -eq 1) 'NoInactiveIdle: a gap over 3 iterations stays real'
    Check ($null -eq $m.Invalid) 'clean load is valid'
    Check ($m.FramesExcluded['first minute'].N -gt 0 -and $m.FramesExcluded['harness step'].N -gt 0 -and $m.Frames.Frames -lt 3601) 'frames in excluded windows leave the frame stats'
    Check ($m.SlowFrames.N -eq 1 -and $m.SlowFrames.Stages[0].Stage -eq 'services' -and $m.SlowFrames.Stages[0].Lead -eq 1 -and $m.SlowFrames.Services[0].Service -eq 'Canvas') 'slow frame led by the services stage, Canvas first'
    Check ((Get-Measurement $base 600 $cmds $quiet).Invalid -match 'cut short') 'profile shorter than 80% is invalid'
    Check ((Get-Measurement $base 120 $cmds @([pscustomobject]@{ t = 90000; game = 30; other = 15 })).Invalid -match 'noisy machine') 'other programs >= 10% make the load invalid'
    Check ((Get-Measurement $base 120 $cmds $quiet -Crash 'crash report written').Invalid -match 'crash') 'a crash makes the load invalid'
    Check ($null -eq (Get-Measurement (Join-Path $fx 'none') 120 @() @())) 'no profile, no measurement'
} finally { Remove-Item -LiteralPath $fx -Recurse -Force -ErrorAction SilentlyContinue }

# Results page
$res = @([pscustomobject]@{ Name = 'x-1-off'; JobId = 'x'; Variant = 'off'; Ok = $true; Closed = 'quit'; Frames = $f; Sim = $null; Notes = @() },
         [pscustomobject]@{ Name = 'x-2-on'; JobId = 'x'; Variant = 'on'; Ok = $false; Closed = 'quit'; Frames = $null; Sim = $null; Notes = @('log: a|b') })
$md = Format-Results $res 'Self-test'
Check ($md -match '\| x-2-on \| FAIL \|' -and $md -match '## x: A/B' -and $md -match 'log: a/b') 'results page'
$res = @([pscustomobject]@{ Name = 'x-1-off'; JobId = 'x'; Variant = 'off'; Ok = $true; Closed = 'quit'; Frames = $m.Frames; Sim = $m.Sim; M = $m; Notes = @() },
         [pscustomobject]@{ Name = 'x-2-on'; JobId = 'x'; Variant = 'on'; Ok = $true; Closed = 'quit'; Frames = $m2.Frames; Sim = $m2.Sim; M = $m2; Notes = @() },
         [pscustomobject]@{ Name = 'x-3-on'; JobId = 'x'; Variant = 'on'; Ok = $true; Closed = 'quit'; Frames = $null; Sim = $null; M = [pscustomobject]@{ RealMin = 1; Invalid = 'noisy machine (x)'; HasFreezes = $false }; Notes = @() })
$md = Format-Results $res 'Self-test'
Check ($md -match '### x-1-off' -and $md -match '\| harness step \| 2 \|' -and $md -match '\| camera_pan \| 1 \|' -and $md -match '\| services \| 1 \|') 'load detail: excluded, harness cost, slow frames'
Check ($md -match '\| iterations \| 300 \| 1, ' -and $md -match '\| visible freezes \| 300 \|' -and $md -match 'not enough events \(3 < 10\)') 'pair rows with intervals and the verdict rule'
Check ($md -match 'Left out \(invalid\): x-3-on \(noisy machine' -and $md -match 'INVALID: noisy') 'invalid loads named and left out of the pair'
$res = @([pscustomobject]@{ Name = 'y-1-off'; JobId = 'y'; Variant = 'off'; Ok = $true; Closed = 'quit'; Frames = $null; Sim = $null; Notes = @(); Info = @('largest free block 900 MB (after the pan)'); FreeMB = 900 },
         [pscustomobject]@{ Name = 'y-2-on'; JobId = 'y'; Variant = 'on'; Ok = $true; Closed = 'quit'; Frames = $null; Sim = $null; Notes = @(); Info = @('1 *.mp4 in captures: a.mp4 1.50 MB'); FreeMB = 880 },
         [pscustomobject]@{ Name = 'y-3-on'; JobId = 'y'; Variant = 'on'; Ok = $true; Closed = 'quit'; Frames = $null; Sim = $null; Notes = @(); FreeMB = 860 })
$md = Format-Results $res 'Self-test'
Check ($md -match '\| y-1-off \| pass \|.*largest free block 900 MB' -and $md -match 'a\.mp4 1\.50 MB' -and $md -match '\| on \| 2 \| 2 \| .* \| 870 \|') 'memory and file readings in the notes, free block mean per variant'

# Results: progress lines go to the host, so a load returns exactly one row; exit code 0 / 1 / 2
function Test-Load { Say 'starting the game'; Say '  test agent: 30 s'; [pscustomobject]@{ Name = 'x-1-on'; Ok = $false } }
$rows = @(Test-Load 6>$null)
Check ($rows.Count -eq 1 -and $rows[0].Name -eq 'x-1-on') 'one result row per load (no status lines in the results)'
$pass = [pscustomobject]@{ Ok = $true }; $fail = [pscustomobject]@{ Ok = $false }
Check ((Get-RunExitCode @($pass, $pass) 2) -eq 0) 'exit 0 when every load passed'
Check ((Get-RunExitCode @($pass, $fail) 2) -eq 1) 'exit 1 when a load failed'
Check ((Get-RunExitCode @($pass) 2) -eq 2 -and (Get-RunExitCode @() 1) -eq 2) 'exit 2 when the run stopped early'
Check ((Get-RunExitCode @($pass, $pass) 2 $true) -eq 2) 'exit 2 when the check after the last load failed'
Check ((Get-RunExitCode @($pass, $pass) 2 $false $true) -eq 3) 'exit 3 when files are still to put back'

# Backup, lock and restore on stand-in game files: an interrupted run, a file that cannot be put back, the job's save
# folder and Apex Radiance's own files
$sb = Join-Path ([IO.Path]::GetTempPath()) ('apex-ingame-selftest-' + [Guid]::NewGuid().ToString('N'))
$held = $null
try {
    $state = Join-Path $sb 'state'; $run = Join-Path $state 'run1'; $game = Join-Path $sb 'game'
    New-Item -ItemType Directory -Path (Join-Path $game 'Saves/Town.sims3'), (Join-Path $game 'Apex Radiance') -Force | Out-Null
    $f = @{ toml = 'Apex Radiance/ApexRadiance.toml'; asi = 'ApexRadiance.asi'; log = 'Apex Radiance/ApexRadiance_LOG.txt'; crash = 'Apex Radiance/ApexRadiance_Crash.txt'
            hitches = 'Apex Radiance/ApexRadiance_Hitches.txt'; cache = 'Apex Radiance/ApexRadiance_ShaderCache.bin'; agent = 'Mods/Packages/ApexTestAgent.package'; save = 'Saves/Town.sims3' }
    $p = @{}; foreach ($k in $f.Keys) { $p[$k] = Join-Path $game $f[$k] }
    foreach ($k in 'toml', 'asi', 'log', 'crash', 'cache') { Set-Content -LiteralPath $p[$k] -Value "player $k" }
    Set-Content -LiteralPath (Join-Path $p.save 'Town.nhd') -Value 'player save'
    $paths = @($p.toml, $p.asi, $p.agent, $p.log, $p.crash, $p.hitches, $p.cache, $p.save)

    New-Item -ItemType Directory -Path $run -Force | Out-Null
    Enter-RunLock $state $run
    $man = New-RunBackup $run $paths
    Check (@($man | Where-Object existed).Count -eq 6 -and @($man | Where-Object folder).Count -eq 1) 'backup lists files, missing files and the save folder'
    $why = $null; try { Enter-RunLock $state (Join-Path $state 'run2') } catch { $why = $_.Exception.Message }
    Check ($why -match 'in progress') 'a second run started mid-run refuses'

    # the test run: everything replaced, new files written, the save touched
    foreach ($k in 'toml', 'asi', 'log', 'crash', 'cache', 'hitches') { Set-Content -LiteralPath $p[$k] -Value "test $k" }
    New-Item -ItemType Directory -Path (Split-Path $p.agent) -Force | Out-Null; Set-Content -LiteralPath $p.agent -Value 'agent'
    Set-Content -LiteralPath (Join-Path $p.save 'Town.nhd') -Value 'overwritten'; Set-Content -LiteralPath (Join-Path $p.save 'extra.tmp') -Value 'x'

    # power loss: the marker stays with a runner process that no longer exists
    $dead = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList '-NoProfile', '-Command', 'exit' -PassThru -WindowStyle Hidden; $dead.WaitForExit()
    $mk = Get-PendingRun $state; $mk.pid = $dead.Id
    Set-Content -LiteralPath (Join-Path $state 'restore-pending.json') -Value ($mk | ConvertTo-Json -Compress)
    $why = $null; try { Enter-RunLock $state (Join-Path $state 'run2') } catch { $why = $_.Exception.Message }
    Check ($why -match [regex]::Escape("-Restore '$run'")) 'after an interrupted run the next run refuses and points to -Restore'

    # a file still held by a closing game: the others are put back, the held one is reported, the lock stays
    $held = [IO.File]::Open($p.asi, 'Open', 'ReadWrite', 'None')
    $r = Restore-RunBackup $run 2 50
    Check ($r.Pending.Count -eq 1 -and $r.Pending[0].path -eq $p.asi -and $r.Restored -eq 7) 'one held file stays pending, the rest are put back'
    Check ((Get-Content -LiteralPath $p.toml) -eq 'player toml' -and -not (Test-Path -LiteralPath $p.agent)) 'settings put back, installed agent removed'
    Check ((Get-Content -LiteralPath (Join-Path $p.save 'Town.nhd')) -eq 'player save' -and -not (Test-Path -LiteralPath (Join-Path $p.save 'extra.tmp'))) 'save folder put back as it was'
    Check ((Get-Content -LiteralPath $p.log) -eq 'player log' -and (Get-Content -LiteralPath $p.crash) -eq 'player crash' -and (Get-Content -LiteralPath $p.cache) -eq 'player cache' -and -not (Test-Path -LiteralPath $p.hitches)) 'Apex log, crash report, shader cache back; new hitch log removed'
    Check ($null -ne (Get-PendingRun $state)) 'lock kept while a file is pending'

    $held.Dispose(); $held = $null
    Set-Content -LiteralPath $p.toml -Value 'player changed it later'
    $r = Restore-RunBackup $run 2 50
    Check ($r.Pending.Count -eq 0 -and $r.Restored -eq 1 -and (Get-Content -LiteralPath $p.asi) -eq 'player asi') 'second restore puts back only what was pending'
    Check ((Get-Content -LiteralPath $p.toml) -eq 'player changed it later') 'entries already put back are not copied again'
    Exit-RunLock $state $run
    Check ($null -eq (Get-PendingRun $state) -and (Restore-RunBackup $run 1 0).Already) 'lock removed; the manifest is used up'
} finally { if ($held) { $held.Dispose() }; Remove-Item -LiteralPath $sb -Recurse -Force -ErrorAction SilentlyContinue }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('apex-ingame-selftest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    # a run that cannot start ends non-zero (here: no TS3W.exe in -GameBin)
    $runArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run.ps1'), '-Job', (Join-Path $PSScriptRoot 'jobs/smoke.json'), '-GameBin', $tmp)
    $p = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList ($runArgs | ForEach-Object { '"' + $_ + '"' }) -Wait -PassThru -WindowStyle Hidden
    Check ($p.ExitCode -ne 0) "run.ps1 exits non-zero when it cannot run (exit $($p.ExitCode))"

    # Start-up watch: observations of a stand-in game (this process), log, marker and mailbox
    $started = (Get-Date).AddSeconds(-5)
    $log = Join-Path $tmp 'ApexRadiance_LOG.txt'
    $o = Get-StartObservation (Get-Process -Id $PID) $log $tmp $started $null
    Check ($o.Alive -and $o.LogBytes -eq -1 -and -not $o.Features -and -not $o.Marker -and -not $o.Found) 'observation before Apex and the agent'
    Set-Content -LiteralPath $log -Value @('12:00:00.000 [INFO] [Main] Loaded as ApexRadiance.asi', '12:00:20.000 [INFO] [Main] Features started')
    Set-Content -LiteralPath (Join-Path $tmp 'ScriptError_1234.xml') -Value '<ApexTestAgent version="1.0.1" stage="loaded" commands="ok" />'
    Set-Content -LiteralPath (Join-Path $tmp 'ScriptError_9999.xml') -Value '<ScriptError>another mod</ScriptError>'
    $o = Get-StartObservation (Get-Process -Id $PID) $log $tmp $started ([pscustomobject]@{ Found = $true; Heartbeat = 7 })
    Check ($o.LogBytes -gt 0 -and $o.Features -and $o.Marker -match 'stage="loaded"' -and $o.Found -and $o.Beat -eq 7) 'observation of log, marker and heartbeat'
    $o = Get-StartObservation (Get-Process -Id $PID) $log $tmp (Get-Date).AddMinutes(1) $null
    Check ($o.LogBytes -eq -1 -and -not $o.Marker) 'files from an earlier start do not count'

    # the rules, one 30 s check per observation
    function Obs([double]$Sec, [double]$Cpu, [long]$Log = 100, [bool]$Features = $false, [string]$Marker = $null, [bool]$Alive = $true) {
        [pscustomobject]@{ Sec = $Sec; Alive = $Alive; ExitCode = 3; Cpu = $Cpu; LogBytes = $Log; Features = $Features; Marker = $Marker; Found = $false; Beat = $null }
    }
    function Run-Watch($List) { $st = @{}; foreach ($x in $List) { $why = Step-StartWatch $st $x; if ($why) { return $why } }; return $null }
    Check ($null -eq (Run-Watch @((Obs 30 5), (Obs 60 9 200 $true), (Obs 90 14 300 $true '<a/>')))) 'healthy start keeps waiting'
    Check ((Run-Watch @((Obs 30 5 -1), (Obs 60 9 -1))) -match 'ApexRadiance.asi did not load') 'no Apex log after 60 s'
    Check ((Run-Watch @((Obs 30 5), (Obs 60 5 0 $false $null $false))) -match 'closed .*exit code 3') 'game closed'
    Check ((Run-Watch @((Obs 30 5), (Obs 60 5.5), (Obs 90 5.9))) -match 'no progress for 60 s') 'two flat checks fail'
    Check ($null -eq (Run-Watch @((Obs 30 5), (Obs 60 5.5), (Obs 90 9)))) 'one flat check is tolerated'
    Check ((Run-Watch @((Obs 30 5 100 $true), (Obs 60 9 200 $true), (Obs 90 13 300 $true), (Obs 120 17 400 $true), (Obs 150 21 500 $true), (Obs 180 25 600 $true), (Obs 210 29 700 $true))) -match 'did not load: no ApexTestAgent marker') 'agent not loaded 180 s after the features'
    Check ((Run-Watch @((Obs 30 5 100 $true), (Obs 210 17 400 $true '<ApexTestAgent stage="loaded" />'))) -match 'loaded \(<ApexTestAgent.*mailbox was not found') 'agent loaded, mailbox not found'
    $noApex = { param($List) $st = @{}; foreach ($x in $List) { $why = Step-StartWatch $st $x -NoApex; if ($why) { return $why } }; return $null }
    Check ($null -eq (& $noApex @((Obs 30 5 -1), (Obs 60 9 -1), (Obs 90 13 -1)))) 'without the mod a missing log is fine'
    Check ((& $noApex @((Obs 120 17 -1), (Obs 150 21 -1), (Obs 180 25 -1), (Obs 210 29 -1), (Obs 240 33 -1), (Obs 270 37 -1), (Obs 300 41 -1))) -match 'did not load: no ApexTestAgent marker') 'without the mod the agent deadline counts from 120 s'
    $st = @{}; $null = Step-StartWatch $st (Obs 30 5 100 $true); $found = Obs 150 30 900 $true; $found.Found = $true; $found.Beat = 4
    Check ($null -eq (Step-StartWatch $st $found)) 'a found mailbox never fails the agent rule'
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }

# Every shipped job: valid JSON, an id equal to its file name, a save, known steps, patterns that compile
foreach ($file in Get-ChildItem (Join-Path $PSScriptRoot 'jobs') -Filter *.json) {
    $j = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    Check ($j.id -eq $file.BaseName -and $j.save -and @(Get-JobLoads $j -AB).Count -ge 1) "$($file.Name): id, save, variants"
    $steps = @(Get-JobSteps $j)
    Check (@($steps | Where-Object { $script:KnownSteps -notcontains $_.do }).Count -eq 0) "$($file.Name): steps"
    Check (@($steps | Where-Object { $_.do -eq 'mode' -and -not $script:ModeStates.Contains([string]$_.mode) }).Count -eq 0) "$($file.Name): mode targets"
    # a job that starts a repro measures while it runs and stops it afterwards (event counts, spawned Sims removed)
    $does = @($steps | ForEach-Object { if ($_.do -eq 'wait' -and $_.measure) { 'measure' } else { $_.do } })
    $iRepro = [array]::IndexOf($does, @($does | Where-Object { $_ -in 'camera_jumps', 'vfx_burst' } | Select-Object -First 1)[0])
    if ($iRepro -ge 0) {
        $iStop = [array]::IndexOf($does, 'stop_repros'); $iMeasure = [array]::IndexOf($does, 'measure'); $iProfile = [array]::IndexOf($does, 'profile')
        Check ($iStop -gt $iRepro -and (($iMeasure -gt $iRepro -and $iStop -gt $iMeasure) -or ($iProfile -ge 0 -and $iProfile -lt $iStop))) "$($file.Name): repro measured, then stop_repros"
    }
    Check (@($does | Where-Object { $_ -eq 'profile' }).Count -le 1) "$($file.Name): at most one profile step"
    foreach ($u in @($steps | Where-Object { $_.until })) {
        $ok = $true; try { $null = Get-UntilCommand @($u.until.PSObject.Properties)[0].Name } catch { $ok = $false }
        Check ($ok -and $u.measure -and @($u.until.PSObject.Properties).Count -eq 1 -and [double]@($u.until.PSObject.Properties)[0].Value -ge 1) "$($file.Name): until is one repro count on a measured wait"
    }
    if ($j.variants.off -and $j.variants.on) { # an off load must prove the feature is off
        Check (@($j.variants.off.fail) -contains @($j.variants.on.expect)[0]) "$($file.Name): off fails on the feature's own start line"
    }
    $patterns = @($j.expect) + @($j.fail) + @($j.variants.PSObject.Properties | ForEach-Object { @($_.Value.expect) + @($_.Value.fail) }) | Where-Object { $_ }
    foreach ($p in $patterns) { $ok = $true; try { [regex]::new($p) | Out-Null } catch { $ok = $false }; Check $ok "$($file.Name): pattern $p" }
}
foreach ($file in Get-ChildItem (Join-Path $PSScriptRoot 'queues') -Filter *.json) {
    $q = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    Check (@($q.jobs | Where-Object { -not (Test-Path (Join-Path $file.DirectoryName $_)) }).Count -eq 0) "$($file.Name): job files exist"
}

# Mailbox: a stand-in agent in this process answers on a thread like the game's poll task; the client finds it by
# scanning this process and exchanges one request and reply
Add-Type -Path (Join-Path $PSScriptRoot 'MailboxClient.cs')
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Threading; using System.Runtime.InteropServices;
public static class FakeAgent {
    static byte[] sBuf; static GCHandle sPin;
    static uint U(int o) { return BitConverter.ToUInt32(sBuf, o); }
    static void P(int o, uint v) { BitConverter.GetBytes(v).CopyTo(sBuf, o); }
    public static void Start() {
        sBuf = new byte[1024 * 1024]; sPin = GCHandle.Alloc(sBuf, GCHandleType.Pinned);
        uint nonce = 12345; P(16, 1); P(20, nonce); P(40, 1024 * 1024); P(44, 1u ^ nonce ^ (1024u * 1024u) ^ 0x41505854u);
        Encoding.ASCII.GetBytes("APEXTEST-MBOX-01").CopyTo(sBuf, 0);
        Thread t = new Thread(delegate() {
            while (true) {
                P(48, U(48) + 1);
                uint req = U(24);
                if (req != U(28)) {
                    string text = Encoding.UTF8.GetString(sBuf, 64, (int)U(32));
                    byte[] reply = Encoding.UTF8.GetBytes("{\"id\":1,\"ok\":true,\"result\":{\"echo\":" + text + "}}");
                    reply.CopyTo(sBuf, 64 + 65536); P(36, (uint)reply.Length); P(28, req);
                }
                Thread.Sleep(5);
            }
        });
        t.IsBackground = true; t.Start();
    }
    public static void Stop() { Array.Clear(sBuf, 0, 64); sPin.Free(); }
}
'@
[FakeAgent]::Start()
$client = New-Object ApexIngame.MailboxClient($PID)
try {
    Check $client.Scan() 'mailbox found by scanning the process'
    $reply = $client.Call('game_state', '{"x":1}', 5000) | ConvertFrom-Json
    Check ($reply.ok -and $reply.result.echo.cmd -eq 'game_state' -and $reply.result.echo.args.x -eq 1) 'request and reply round trip'
    $b1 = $client.Heartbeat; Start-Sleep -Milliseconds 100
    Check ($client.Heartbeat -ne $b1) 'heartbeat moves'
    $free = $client.LargestFree()
    Check ($free -gt 1MB -and $free -le 0xFFFF0000L) "largest free block read below 4 GB ($([math]::Round($free / 1MB)) MB)"
} finally { $client.Dispose(); [FakeAgent]::Stop() }

Write-Output "$($script:checks) checks, $($script:failures) failures"
if ($script:failures) { exit 1 }

# In-game runner: installs a built ApexRadiance.asi and the test agent, starts the game once per load with that load's
# ApexRadiance.toml, loads a test save, drives the job's steps, collects Apex's own outputs (and optional frame times),
# closes the game and writes results.md. Everything installed is put back afterwards. See README.md.
# Adapted from sims3-modernization tools/harness (MIT, see LICENSE-sims3-modernization).
#
#   ./tools/ingame_runner/run.ps1 -GameBin '<Game>\Bin' -Job tools/ingame_runner/jobs/smoke.json
#   ./tools/ingame_runner/run.ps1 -GameBin '<Game>\Bin' -Job tools/ingame_runner/jobs/resource-lookup-cache.json -AB -Profiler Frames
#   ./tools/ingame_runner/run.ps1 -GameBin '<Game>\Bin' -Queue tools/ingame_runner/queues/performance.json -Profiler Frames
#   ./tools/ingame_runner/run.ps1 -Restore outputs/ingame/<run>            (puts a run's backup back after a power loss)
param(
    [string[]]$Job,
    [string]$Queue,
    [string]$GameBin,
    [string]$Variant,
    [switch]$AB,
    [ValidateSet('None', 'Frames', 'Sim', 'Both')][string]$Profiler = 'None',
    [string]$Asi,
    [string]$AsiLoader,
    [string]$GameDocuments = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Electronic Arts\The Sims 3'),
    [string]$OutDir,
    [int]$LoadTimeoutMin = 20,
    [switch]$KeepSaves,
    [switch]$DryRun,
    [string]$Restore
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $PSScriptRoot 'runner_lib.ps1')
$apexDir = Join-Path $GameDocuments 'Apex Radiance'
$tomlPath = Join-Path $apexDir 'ApexRadiance.toml'

# ------------------------------------------------------------------ backup and restore
# The manifest lists every file the run may replace or remove, with whether it existed. Restore copies each back, or
# deletes it when it did not exist before the run.
function Restore-Backup([string]$RunDir) {
    $manifestPath = Join-Path $RunDir 'backup/manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw "No backup manifest in $RunDir" }
    foreach ($e in (Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json)) {
        if ($e.existed) { Copy-Item -LiteralPath (Join-Path $RunDir "backup/$($e.copy)") -Destination $e.path -Force }
        elseif (Test-Path -LiteralPath $e.path) { Remove-Item -LiteralPath $e.path -Force }
    }
    Say "restored $((Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json).Count) files from $RunDir\backup"
}

if ($Restore) { Restore-Backup (Resolve-Path $Restore).Path; return }

# ------------------------------------------------------------------ jobs and checks before touching anything
if ($Queue) {
    $queueDir = Split-Path (Resolve-Path $Queue).Path
    $q = Get-Content -LiteralPath $Queue -Raw | ConvertFrom-Json
    $Job = @($q.jobs | ForEach-Object { Join-Path $queueDir $_ })
    if ($q.ab) { $AB = [switch]$true }
}
if (-not $Job) { throw 'Pass -Job <file.json> (one or more) or -Queue <file.json>.' }
$jobs = @($Job | ForEach-Object { $j = Get-Content -LiteralPath $_ -Raw | ConvertFrom-Json; if (-not $j.id) { throw "$_ has no id" }; $j })
$loads = @($jobs | ForEach-Object { Get-JobLoads $_ -Variant $Variant -AB:$AB })

if (-not $GameBin) { throw "Pass -GameBin '<Game>\Bin' (the folder with TS3W.exe)." }
$exe = Join-Path $GameBin 'TS3W.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "TS3W.exe not found in $GameBin" }
if (-not $Asi) { $Asi = @('Public/ApexRadiance.asi', 'Release/ApexRadiance.asi') | ForEach-Object { Join-Path $repo $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not $Asi -or -not (Test-Path -LiteralPath $Asi)) { throw 'No built ApexRadiance.asi: run tools/development/build.ps1 or pass -Asi.' }
$agent = Join-Path $repo 'outputs/ingame/agent/ApexTestAgent.package'
if (-not (Test-Path -LiteralPath $agent)) { throw "No test agent: run node tools/ingame_runner/build_agent.mjs -GameBin '$GameBin'." }
$savesDir = Join-Path $GameDocuments 'Saves'
if (-not (Test-Path -LiteralPath (Join-Path $GameDocuments 'Mods/Resource.cfg'))) { throw "No Mods\Resource.cfg in ${GameDocuments}: set up the Mods folder for script mods first (README)." }
foreach ($j in $jobs) { if (-not (Test-Path -LiteralPath (Join-Path $savesDir "$($j.save).sims3"))) { throw "Job $($j.id): save '$($j.save).sims3' not found in $savesDir" } }
if ($Profiler -ne 'None') {
    & python -c 'import frida' 2>$null
    if ($LASTEXITCODE -ne 0) { throw "-Profiler $Profiler needs Python with the frida package (pip install frida)." }
}

if ($DryRun) {
    Say "dry run: $($loads.Count) load(s), asi $Asi"
    $base = if (Test-Path -LiteralPath $tomlPath) { [IO.File]::ReadAllText($tomlPath) } else { '' }
    foreach ($l in $loads) {
        $v = $l.Job.variants.($l.Variant)
        $toml = Set-TomlOverrides (Set-TomlOverrides $base $l.Job.toml) $v.toml
        Say "$($l.Name): save $($l.Job.save), steps $(@(Get-JobSteps $l.Job).Count), toml changes:"
        Compare-Object @($base -split "`r?`n") @($toml -split "`r?`n") | Where-Object { $_.SideIndicator -eq '=>' -and $_.InputObject } | ForEach-Object { Write-Output "    $($_.InputObject)" }
    }
    return
}

$running = @(Get-Process -Name 'TS3W', 'Sims3LauncherW' -ErrorAction SilentlyContinue)
if ($running) { throw 'The game or its launcher is running. The runner never touches a game in use: close it first.' }
if (-not (Get-Process -Name 'steam' -ErrorAction SilentlyContinue)) { Say 'warning: Steam is not running; the Steam game needs it to start' }
$loaders = @('wininet.dll', 'dinput8.dll', 'version.dll', 'winmm.dll', 'dsound.dll') | Where-Object { Test-Path (Join-Path $GameBin $_) }
if (-not $AsiLoader -and -not $loaders) { Say 'warning: no ASI loader found in Bin (for example Ultimate ASI Loader as wininet.dll); pass -AsiLoader' }

Add-Type -Path (Join-Path $PSScriptRoot 'MailboxClient.cs')
if (-not $OutDir) { $OutDir = Join-Path $repo ('outputs/ingame/' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Path (Join-Path $OutDir 'backup') -Force | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# ------------------------------------------------------------------ game control
$script:proc = $null
$script:mbox = $null
$script:speed = 'normal'

function Call-Agent([string]$Cmd, $ArgsObject, [int]$TimeoutMs = 15000) {
    $json = if ($null -ne $ArgsObject) { $ArgsObject | ConvertTo-Json -Compress -Depth 6 } else { '' }
    $reply = $script:mbox.Call($Cmd, $json, $TimeoutMs) | ConvertFrom-Json
    if (-not $reply.ok) { throw "agent $Cmd failed: $($reply.error)" }
    return $reply.result
}

# Waits until $Done returns true. Every 30 s the game must show progress (simulator ticks, or CPU time while loading);
# two checks in a row without progress, a closed game or the time limit fail the wait with the reason.
function Wait-Game([string]$What, [double]$Minutes, [scriptblock]$Done, [scriptblock]$Each) {
    $end = (Get-Date).AddMinutes($Minutes)
    $nextCheck = (Get-Date).AddSeconds(30)
    $misses = 0
    $cpu = $script:proc.TotalProcessorTime.TotalSeconds
    $beat = $null
    while ($true) {
        if ($script:proc.HasExited) { throw "game closed while waiting for $What (exit code $($script:proc.ExitCode))" }
        if (& $Done) { return }
        if ((Get-Date) -ge $end) { throw "timeout after $Minutes min waiting for $What" }
        if ((Get-Date) -ge $nextCheck) {
            $nextCheck = (Get-Date).AddSeconds(30)
            $script:proc.Refresh()
            $nowCpu = $script:proc.TotalProcessorTime.TotalSeconds
            $nowBeat = $null
            if ($script:mbox -and $script:mbox.Found) { try { $nowBeat = $script:mbox.Heartbeat } catch { } }
            $progress = ($nowCpu - $cpu -gt 1) -or ($null -ne $nowBeat -and $nowBeat -ne $beat)
            $cpu = $nowCpu; $beat = $nowBeat
            if ($progress) { $misses = 0 } else { $misses++ }
            Say ("  {0}: {1:N1} min left, cpu {2:N0} s, ticks {3}" -f $What, ($end - (Get-Date)).TotalMinutes, $nowCpu, $nowBeat)
            if ($misses -ge 2) { throw "no progress for 60 s while waiting for $What" }
            if ($Each) { & $Each }
        }
        Start-Sleep -Milliseconds 1000
    }
}

# Waits for the test agent's mailbox after the game starts. Scans the game's memory every 10 s; every 30 s
# Step-StartWatch (runner_lib.ps1) checks the process, Apex's log, the agent's marker file and heartbeat and stops the
# load with the reason; -LoadTimeoutMin is only the last resort.
function Wait-Agent([datetime]$Started) {
    $state = @{}
    $logPath = Join-Path $apexDir 'ApexRadiance_LOG.txt'
    $lastScan = [datetime]::MinValue
    $nextCheck = $Started.AddSeconds(30)
    $end = $Started.AddMinutes($LoadTimeoutMin)
    while (-not $script:mbox.Found) {
        if ($script:proc.HasExited) { throw "the game closed before the test agent answered (exit code $($script:proc.ExitCode))" }
        if (((Get-Date) - $lastScan).TotalSeconds -ge 10) { $lastScan = Get-Date; if ($script:mbox.Scan()) { break } }
        if ((Get-Date) -ge $nextCheck) {
            $nextCheck = (Get-Date).AddSeconds(30)
            $o = Get-StartObservation $script:proc $logPath $GameDocuments $Started $script:mbox
            Say ("  test agent: {0:N0} s since start, cpu {1:N0} s, Apex log {2}, features {3}, agent marker {4}" -f $o.Sec, $o.Cpu,
                $(if ($o.LogBytes -ge 0) { "$($o.LogBytes) bytes" } else { 'none yet' }), $(if ($o.Features) { 'started' } else { 'not yet' }),
                $(if ($o.Marker) { $o.Marker } else { 'none yet' }))
            $why = Step-StartWatch $state $o
            if ($why) { throw $why }
        }
        if ((Get-Date) -ge $end) { throw "timeout after $LoadTimeoutMin min waiting for the test agent" }
        Start-Sleep -Seconds 1
    }
    Say "  test agent found $([math]::Round(((Get-Date) - $Started).TotalSeconds)) s after the start"
}

# "Mod Scripts Found" (the game's warning when script mods are installed) opens at the main menu up to ~90 s after the
# agent answers, and accepting it during a load leaves the game on the loading screen (sims3-modernization cycle.ts).
# Answers menu dialogs until that one was seen and the menu stayed quiet for 5 s, or for 120 s without it.
function Wait-MenuQuiet {
    $m = @{ Start = Get-Date; Quiet = Get-Date; Saw = $false } # a hashtable: the script block below cannot assign outer variables
    Wait-Game 'the "Mod Scripts Found" dialog' 3 {
        try { $d = Call-Agent 'auto_dialog' $null 5000 } catch { return $false }
        if ($d.open) { Say "  menu dialog '$($d.title)': clicked $($d.clicked)"; if ($d.title -match 'mod scripts') { $m.Saw = $true }; $m.Quiet = Get-Date }
        if ($m.Saw) { return ((Get-Date) - $m.Quiet).TotalSeconds -ge 5 }
        return ((Get-Date) - $m.Start).TotalSeconds -ge 120
    }
    if (-not $m.Saw) { Say '  no "Mod Scripts Found" dialog within 120 s; loading anyway' }
}

function Clear-Dialog { try { $d = Call-Agent 'auto_dialog' $null; if ($d.clicked) { Say "  dialog '$($d.title)': clicked $($d.clicked)" } } catch { Say "  dialog: $($_.Exception.Message)" } }

function Keep-Speed {
    Clear-Dialog
    try { $s = Call-Agent 'game_state' $null; if ($script:speed -ne 'pause' -and $s.speed -eq 'Pause') { Call-Agent 'set_speed' @{ speed = $script:speed } | Out-Null } } catch { }
}

function Wait-LiveMode([string]$What) {
    Wait-Game $What $LoadTimeoutMin {
        try { $s = Call-Agent 'game_state' $null 5000; return ($s.in_world -and $s.sub_state -eq 'LiveModeState' -and -not $s.modal) } catch { return $false }
    } { Clear-Dialog }
}

function Start-Profilers([string]$Base, [double]$Seconds) {
    if ($Profiler -eq 'None') { return $null }
    $pargs = @((Join-Path $PSScriptRoot 'profilers/attach.py'), $script:proc.Id, $Seconds)
    if ($Profiler -in 'Frames', 'Both') { $pargs += @((Join-Path $PSScriptRoot 'profilers/frame-times.js'), (@{ out = ("$Base-frames.csv" -replace '\\', '/') } | ConvertTo-Json -Compress)) }
    if ($Profiler -in 'Sim', 'Both') { $pargs += @((Join-Path $PSScriptRoot 'profilers/stutter-profile.js'), (@{ out = ($Base -replace '\\', '/'); hitchMs = 50 } | ConvertTo-Json -Compress)) }
    return Start-Process -FilePath 'python' -ArgumentList ($pargs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -NoNewWindow -PassThru `
        -RedirectStandardOutput "$Base-profiler.log" -RedirectStandardError "$Base-profiler.err.log"
}

# ------------------------------------------------------------------ one load
function Invoke-Load($Load, [string]$BaseToml) {
    $jobDef = $Load.Job
    $v = $jobDef.variants.($Load.Variant)
    $dir = Join-Path $OutDir $Load.Name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $notes = New-Object System.Collections.Generic.List[string]
    $result = [pscustomobject]@{ Name = $Load.Name; JobId = $jobDef.id; Variant = $Load.Variant; Ok = $false; Closed = ''; Frames = $null; Sim = $null; Notes = $notes; Saves = @() }

    $toml = Set-TomlOverrides (Set-TomlOverrides $BaseToml $jobDef.toml) $v.toml
    New-Item -ItemType Directory -Path $apexDir -Force | Out-Null
    [IO.File]::WriteAllText($tomlPath, $toml, (New-Object Text.UTF8Encoding($false)))
    Copy-Item -LiteralPath $tomlPath -Destination (Join-Path $dir 'ApexRadiance.toml')

    $started = Get-Date
    $script:speed = 'normal'
    $measuredMin = 0
    $gameArgs = @($jobDef.gameArgs | Where-Object { $_ })
    Say "$($Load.Name): starting the game"
    if ($gameArgs) { $script:proc = Start-Process -FilePath $exe -WorkingDirectory $GameBin -ArgumentList $gameArgs -PassThru }
    else { $script:proc = Start-Process -FilePath $exe -WorkingDirectory $GameBin -PassThru }
    try {
        $script:mbox = New-Object ApexIngame.MailboxClient($script:proc.Id)
        Wait-Agent $started
        Wait-Game 'the main menu' 5 { try { Call-Agent 'game_state' $null 5000 | Out-Null; return $true } catch { return $false } }
        Wait-MenuQuiet
        Call-Agent 'load_save' @{ file = "$($jobDef.save).sims3" } | Out-Null
        Say "$($Load.Name): loading $($jobDef.save)"
        Wait-LiveMode "the save $($jobDef.save)"
        $lastSave = $null
        $n = 0
        foreach ($step in (Get-JobSteps $jobDef)) {
            $n++
            switch ($step.do) {
                'speed' { $script:speed = $step.speed; Call-Agent 'set_speed' @{ speed = $step.speed } | Out-Null }
                'camera' { Call-Agent 'camera' @{ pos = $step.pos; target = $step.target } | Out-Null }
                'camera_pan' {
                    $a = @{}; foreach ($p in $step.PSObject.Properties) { if ($p.Name -ne 'do') { $a[$p.Name] = $p.Value } }
                    $pan = Call-Agent 'camera_pan' $a
                    Say "  camera pan over $($pan.lots) lots, $($pan.passes) passes of $([math]::Round($pan.passMs / 1000)) s"
                }
                'wait' {
                    $seconds = [double]$step.minutes * 60
                    $prof = $null
                    if ($step.measure) { $prof = Start-Profilers (Join-Path $dir "measure$n") $seconds; $measuredMin += [double]$step.minutes }
                    Say "  step ${n}: wait $($step.minutes) min$(if ($step.measure) { ' (measured)' })"
                    $until = (Get-Date).AddSeconds($seconds)
                    Wait-Game "step $n (wait)" ([double]$step.minutes + 2) { (Get-Date) -ge $until } { Keep-Speed }
                    if ($prof) {
                        if (-not $prof.WaitForExit(90000)) { $prof.Kill(); $notes.Add('profiler did not stop in time and was ended') }
                        elseif ($prof.ExitCode -ne 0) { $notes.Add("profiler exit $($prof.ExitCode), see measure$n-profiler.err.log") }
                    }
                }
                'save' {
                    $lastSave = 'ApexRunner-{0}-{1}' -f $Load.Name, $n
                    $result.Saves += $lastSave
                    Call-Agent 'save_game' @{ name = $lastSave } | Out-Null
                    Say "  step ${n}: saving as $lastSave"
                    Start-Sleep -Seconds 5
                    Wait-Game "save $lastSave" 15 {
                        try { $s = Call-Agent 'game_state' $null 5000; return (-not $s.save_in_progress -and (Test-Path -LiteralPath (Join-Path $savesDir "$lastSave.sims3"))) } catch { return $false }
                    }
                }
                'reload' {
                    $file = if ($step.save -and $step.save -ne 'last') { "$($step.save).sims3" } elseif ($lastSave) { "$lastSave.sims3" } else { throw 'reload: no save made yet' }
                    Say "  step ${n}: reloading $file"
                    Call-Agent 'load_save' @{ file = $file; reload = $true } | Out-Null
                    Start-Sleep -Seconds 10
                    Wait-LiveMode "reload of $file"
                }
                default { throw "unknown step '$($step.do)'" }
            }
        }
        Say "$($Load.Name): closing the game"
        try { Call-Agent 'quit_game' $null | Out-Null } catch { $notes.Add("quit_game: $($_.Exception.Message)") }
        if ($script:proc.WaitForExit(120000)) { $result.Closed = 'quit' }
        elseif ($script:proc.CloseMainWindow() -and $script:proc.WaitForExit(60000)) { $result.Closed = 'window closed' }
        else { Stop-Process -Id $script:proc.Id -Force; $result.Closed = 'ended'; $notes.Add('the game did not close and was ended') }
    } catch {
        $notes.Add($_.Exception.Message)
        if ($script:proc.HasExited) { $result.Closed = "exited $($script:proc.ExitCode)" }
        else { Stop-Process -Id $script:proc.Id -Force; $result.Closed = 'ended after a failure' }
    } finally {
        if ($script:mbox) { $script:mbox.Dispose(); $script:mbox = $null }
    }
    Wait-Process -Id $script:proc.Id -Timeout 60 -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2 # Apex's log writer and crash report finish with the process

    foreach ($f in 'ApexRadiance_LOG.txt', 'ApexRadiance_Crash.txt', 'ApexRadiance_Hitches.txt') {
        $p = Join-Path $apexDir $f
        if ((Test-Path -LiteralPath $p) -and (Get-Item -LiteralPath $p).LastWriteTime -ge $started) { Copy-Item -LiteralPath $p -Destination $dir }
    }
    if (Test-Path (Join-Path $dir 'ApexRadiance_Crash.txt')) { $notes.Add('crash report written') }
    # the agent's marker files (Marker in agent/Agent.cs) belong to the run, not to the player's folder
    Get-ChildItem -LiteralPath $GameDocuments -Filter 'ScriptError*.xml' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $started -and (Select-String -LiteralPath $_.FullName -SimpleMatch '<ApexTestAgent ' -List) } |
        Move-Item -Destination $dir -Force
    $logPath = Join-Path $dir 'ApexRadiance_LOG.txt'
    $lines = if (Test-Path -LiteralPath $logPath) { @(Get-Content -LiteralPath $logPath) } else { $notes.Add('no ApexRadiance_LOG.txt from this load'); @() }
    $check = Test-ApexLog $lines (@($jobDef.expect) + @($v.expect)) ($script:DefaultFailPatterns + @($jobDef.fail) + @($v.fail))
    foreach ($m in $check.Missing) { $notes.Add("missing log line: $m") }
    foreach ($m in $check.Failures) { $notes.Add("log: $m") }
    # shortcut: only the first measured wait counts; sum several measured steps when a job needs it
    $frames = Get-ChildItem -Path $dir -Filter 'measure*-frames.csv' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($frames) { $result.Frames = Get-FrameStats $frames.FullName }
    $simBase = Get-ChildItem -Path $dir -Filter 'measure*-hitches.jsonl' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($simBase) { $result.Sim = Get-SimStats ($simBase.FullName -replace '-hitches\.jsonl$', '') $measuredMin }
    $result.Ok = $check.Ok -and ($result.Closed -in 'quit', 'window closed') -and -not (Test-Path (Join-Path $dir 'ApexRadiance_Crash.txt')) -and
        -not ($notes | Where-Object { $_ -notmatch '^(log|missing log line):' -and $_ -notmatch 'profiler' })
    if (-not $KeepSaves) { foreach ($s in $result.Saves) { $p = Join-Path $savesDir "$s.sims3"; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } } }
    Say "$($Load.Name): $(if ($result.Ok) { 'pass' } else { 'FAIL' })$(if ($notes.Count) { ' - ' + ($notes -join '; ') })"
    return $result
}

# ------------------------------------------------------------------ install, run, restore
$installs = @(
    @{ path = (Join-Path $GameBin 'ApexRadiance.asi'); from = $Asi },
    @{ path = (Join-Path $GameDocuments 'Mods/Packages/ApexTestAgent.package'); from = $agent },
    @{ path = (Join-Path $GameDocuments 'scriptCache.package'); from = $null }, # stale compiled scripts; the game rebuilds it
    @{ path = $tomlPath; from = $null }
)
if ($AsiLoader) { $installs += @{ path = (Join-Path $GameBin 'wininet.dll'); from = $AsiLoader } }
$manifest = @()
$i = 0
foreach ($e in $installs) {
    $i++
    $existed = Test-Path -LiteralPath $e.path
    $copy = "$i-" + (Split-Path $e.path -Leaf)
    if ($existed) { Copy-Item -LiteralPath $e.path -Destination (Join-Path $OutDir "backup/$copy") }
    $manifest += [pscustomobject]@{ path = $e.path; existed = $existed; copy = $copy }
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutDir 'backup/manifest.json') -Encoding UTF8
Say "backup of $($manifest.Count) files in $OutDir\backup (restore by hand: run.ps1 -Restore '$OutDir')"
$baseToml = if (Test-Path -LiteralPath $tomlPath) { [IO.File]::ReadAllText($tomlPath) } else { '' }
$results = @()
$stopped = $null
try {
    foreach ($e in $installs) {
        if ($e.from) { New-Item -ItemType Directory -Path (Split-Path $e.path) -Force | Out-Null; Copy-Item -LiteralPath $e.from -Destination $e.path -Force }
        elseif ($e.path -like '*scriptCache.package' -and (Test-Path -LiteralPath $e.path)) { Remove-Item -LiteralPath $e.path -Force }
    }
    Say "installed $(Split-Path $Asi -Leaf) ($((Get-FileHash -LiteralPath $Asi -Algorithm SHA256).Hash.Substring(0, 12))) and the test agent; $($loads.Count) load(s)"
    foreach ($l in $loads) {
        $row = @(Invoke-Load $l $baseToml)
        if ($row.Count -ne 1) { throw "internal: Invoke-Load returned $($row.Count) values instead of one result" }
        $results += $row[0]
        if (Get-Process -Name 'TS3W' -ErrorAction SilentlyContinue) { throw 'TS3W.exe still running after a load; stopping the run' }
    }
} catch {
    $stopped = $_.Exception.Message
    Say "run stopped: $stopped"
} finally {
    if ($script:proc -and -not $script:proc.HasExited) { Stop-Process -Id $script:proc.Id -Force; Wait-Process -Id $script:proc.Id -Timeout 60 -ErrorAction SilentlyContinue }
    Restore-Backup $OutDir
    if ($results) {
        $title = 'In-game run ' + (Split-Path $OutDir -Leaf)
        [IO.File]::WriteAllText((Join-Path $OutDir 'results.md'), (Format-Results $results $title), (New-Object Text.UTF8Encoding($false)))
        $results | Select-Object Name, JobId, Variant, Ok, Closed, Frames, Sim, Notes | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutDir 'results.json') -Encoding UTF8
        Say "results: $OutDir\results.md ($(@($results | Where-Object Ok).Count) of $($results.Count) loads passed)"
    }
}
# 0 all loads passed, 1 a load failed, 2 the run stopped early (see runner_lib.ps1 Get-RunExitCode)
exit (Get-RunExitCode $results $loads.Count)

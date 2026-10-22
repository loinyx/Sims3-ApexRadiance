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
    [switch]$InactiveIdle,
    [switch]$Lean,
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
Add-Type -Namespace ApexIngame -Name SystemTimes -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);'
if (-not $OutDir) { $OutDir = Join-Path $repo ('outputs/ingame/' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Path (Join-Path $OutDir 'backup') -Force | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# ------------------------------------------------------------------ game control
$script:proc = $null
$script:mbox = $null
$script:speed = 'normal'
$script:commands = $null # agent calls and job steps of the current load, QPC ms (runner_lib.ps1 Get-Measurement)

# QueryPerformanceCounter in ms: the clock the profilers stamp with, so commands and profiles line up exactly
function Get-QpcMs { [Diagnostics.Stopwatch]::GetTimestamp() * 1000.0 / [Diagnostics.Stopwatch]::Frequency }

function Call-Agent([string]$Cmd, $ArgsObject, [int]$TimeoutMs = 15000) {
    $json = if ($null -ne $ArgsObject) { $ArgsObject | ConvertTo-Json -Compress -Depth 6 } else { '' }
    $t0 = Get-QpcMs
    try { $reply = $script:mbox.Call($Cmd, $json, $TimeoutMs) | ConvertFrom-Json }
    finally { if ($null -ne $script:commands) { $script:commands.Add([pscustomobject]@{ label = $Cmd; kind = 'call'; t0 = $t0; t1 = (Get-QpcMs) }) } }
    if (-not $reply.ok) { throw "agent $Cmd failed: $($reply.error)" }
    return $reply.result
}

# CPU of the game and of everything else, as a share of the whole machine since the previous sample
$script:cpuPrev = $null
function Add-MachineSample($List) {
    $idle = 0L; $kernel = 0L; $user = 0L
    if (-not [ApexIngame.SystemTimes]::GetSystemTimes([ref]$idle, [ref]$kernel, [ref]$user)) { return }
    $script:proc.Refresh()
    $now = @{ T = (Get-QpcMs); Total = $kernel + $user; Busy = $kernel + $user - $idle; Game = $script:proc.TotalProcessorTime.Ticks }
    $p = $script:cpuPrev; $script:cpuPrev = $now
    if (-not $p -or $now.Total -le $p.Total) { return }
    $total = [double]($now.Total - $p.Total)
    $game = 100 * ($now.Game - $p.Game) / $total
    $List.Add([pscustomobject]@{ t = $now.T; game = [math]::Round($game, 1); other = [math]::Round([math]::Max(0, 100 * ($now.Busy - $p.Busy) / $total - $game), 1) })
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
            if ($script:window -and (Get-Date) -lt $script:window.End) { Add-MachineSample $script:window.Machine }
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

# Sim: stutter-profile.js, unloaded at the end of the window. Frames: frame-times.js in its own session, its file closed at
# the end of the window and the session kept loaded until the game exits (attach.py --keep). Returns the processes.
function Start-Profilers([string]$Base, [double]$Seconds) {
    $started = @{}
    $attach = Join-Path $PSScriptRoot 'profilers/attach.py'
    $launch = {
        param($Name, $PyArgs)
        Start-Process -FilePath 'python' -ArgumentList ($PyArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -NoNewWindow -PassThru `
            -RedirectStandardOutput "$Base-$Name.log" -RedirectStandardError "$Base-$Name.err.log"
    }
    if ($Profiler -in 'Sim', 'Both') {
        $opts = @{ out = ($Base -replace '\\', '/'); hitchMs = 50 }
        if ($Lean) { $opts.stages = $false }
        $started.Sim = & $launch 'profiler' @($attach, '--stop', "$Base.stop", $script:proc.Id, $Seconds, (Join-Path $PSScriptRoot 'profilers/stutter-profile.js'), ($opts | ConvertTo-Json -Compress))
    }
    if ($Profiler -in 'Frames', 'Both') {
        $started.Frames = & $launch 'frames' @($attach, '--keep', '--stop', "$Base.stop", $script:proc.Id, $Seconds, (Join-Path $PSScriptRoot 'profilers/frame-times.js'),
            (@{ out = ("$Base-frames.csv" -replace '\\', '/') } | ConvertTo-Json -Compress))
    }
    return $started
}

# The measured window: profilers for $Seconds and a machine sample every 30 s (Wait-Game) while it lasts.
$script:window = $null
function Start-Measured([string]$Base, [double]$Seconds, $Machine) {
    $script:cpuPrev = $null
    Remove-Item -LiteralPath "$Base.stop" -ErrorAction SilentlyContinue
    $m = @{ Base = $Base; Seconds = $Seconds; Start = (Get-Date); End = (Get-Date).AddSeconds($Seconds); Machine = $Machine; Done = $false }
    $m.Prof = Start-Profilers $Base $Seconds
    Add-MachineSample $Machine
    $script:window = $m
    return $m
}

# until: { "<field>": N } of camera_jumps (jumps, loops, outfitChanges, spawned) or vfx_burst (rounds, started), read from
# the repro's status. Decided before the run and independent of the on/off difference, so stopping there biases nothing.
function Get-UntilCount($Until) {
    $p = @($Until.PSObject.Properties)[0]
    $cmd = Get-UntilCommand $p.Name
    $s = Call-Agent $cmd @{ status = $true } 5000
    $have = [double]$s.($p.Name)
    [pscustomobject]@{ Reached = ($have -ge [double]$p.Value); Text = "$($p.Name) $have of $($p.Value)" }
}

# Waits for the window's end (the game keeps running at its speed), then for the simulation profiler to unload.
function Complete-Measured($M, $Machine, $Notes) {
    if ($M.Done) { return }
    if ((Get-Date) -lt $M.End) {
        Say "  measured window: $([math]::Ceiling(($M.End - (Get-Date)).TotalSeconds)) s left"
        Wait-Game 'the end of the measured window' (($M.End - (Get-Date)).TotalMinutes + 2) { (Get-Date) -ge $M.End } { Keep-Speed }
    }
    Add-MachineSample $Machine
    $script:window = $null; $M.Done = $true
    if ($M.Prof.Sim) {
        if (-not $M.Prof.Sim.WaitForExit(90000)) { $M.Prof.Sim.Kill(); $Notes.Add('profiler did not stop in time and was ended') }
        elseif ($M.Prof.Sim.ExitCode -ne 0) { $Notes.Add("profiler exit $($M.Prof.Sim.ExitCode), see $(Split-Path $M.Base -Leaf)-profiler.err.log") }
    }
}

# ------------------------------------------------------------------ one load
function Invoke-Load($Load, [string]$BaseToml) {
    $jobDef = $Load.Job
    $v = $jobDef.variants.($Load.Variant)
    $dir = Join-Path $OutDir $Load.Name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $notes = New-Object System.Collections.Generic.List[string]
    $result = [pscustomobject]@{ Name = $Load.Name; JobId = $jobDef.id; Variant = $Load.Variant; Ok = $false; Closed = ''; Frames = $null; Sim = $null; M = $null; Events = $null; Notes = $notes; Saves = @() }
    $script:commands = New-Object System.Collections.Generic.List[object]
    $machine = New-Object System.Collections.Generic.List[object]
    $measured = $null # the measured window (a profile step or the first measured wait): base, seconds, profilers
    $script:window = $null

    $toml = Set-TomlOverrides (Set-TomlOverrides $BaseToml $jobDef.toml) $v.toml
    New-Item -ItemType Directory -Path $apexDir -Force | Out-Null
    [IO.File]::WriteAllText($tomlPath, $toml, (New-Object Text.UTF8Encoding($false)))
    Copy-Item -LiteralPath $tomlPath -Destination (Join-Path $dir 'ApexRadiance.toml')

    $started = Get-Date
    $script:speed = 'normal'
    $gameArgs = @($jobDef.gameArgs | Where-Object { $_ })
    # The game sleeps 10 ms per main-loop pass while another window is in front unless the NoInactiveIdle key is in its
    # [Config] settings; the command line fills that section (-Key or -Key=value), Default.ini's [Config] does not reach
    # this key. The runner never focuses the game window, so without it every frame and publish gap of a measured load
    # would carry that sleep. -InactiveIdle keeps the game's default.
    if (-not $InactiveIdle) { $gameArgs += '-NoInactiveIdle' }
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
            $stepAt = $script:commands.Count; $stepT0 = Get-QpcMs
            switch ($step.do) {
                'speed' { $script:speed = $step.speed; Call-Agent 'set_speed' @{ speed = $step.speed } | Out-Null }
                'camera' { Call-Agent 'camera' @{ pos = $step.pos; target = $step.target } | Out-Null }
                'camera_pan' {
                    $a = @{}; foreach ($p in $step.PSObject.Properties) { if ($p.Name -ne 'do') { $a[$p.Name] = $p.Value } }
                    $pan = Call-Agent 'camera_pan' $a
                    Say "  camera pan over $($pan.lots) lots, $($pan.passes) passes of $([math]::Round($pan.passMs / 1000)) s"
                }
                'profile' { # starts the measured window and goes on with the next steps; the load waits for its end after the last step
                    if ($measured) { throw 'profile: a load has one measured window' }
                    $measured = Start-Measured (Join-Path $dir "measure$n") ([double]$step.minutes * 60) $machine
                    Say "  step ${n}: profiling $($step.minutes) min in the background"
                }
                'wait' {
                    $seconds = [double]$step.minutes * 60
                    $first = $step.measure -and -not $measured # shortcut: only the first measured wait counts; sum several when a job needs it
                    if ($first) { $measured = Start-Measured (Join-Path $dir "measure$n") $seconds $machine }
                    Say "  step ${n}: wait $($step.minutes) min$(if ($first) { ' (measured)' })$(if ($step.until) { ', or until ' + (Format-Until $step.until) })"
                    $until = (Get-Date).AddSeconds($seconds)
                    $poll = if ($step.until -and $first) { { # every 30 s: the repro's own count against the count set in the job
                        $c = Get-UntilCount $step.until
                        if ($c.Reached -and -not $script:stopAt) {
                            $script:stopAt = Get-Date; Set-Content -LiteralPath "$($measured.Base).stop" -Value $c.Text
                            $result.Events = "stopped at $($c.Text) after $([math]::Round(($script:stopAt - $measured.Start).TotalMinutes, 1)) min"
                            Say "  $($result.Events)"
                        }
                    } } else { $null }
                    $script:stopAt = $null
                    Wait-Game "step $n (wait)" ([double]$step.minutes + 2) { (Get-Date) -ge $until -or $script:stopAt } { Keep-Speed; if ($poll) { & $poll } }
                    if ($first) {
                        if ($script:stopAt) { $measured.Seconds = ($script:stopAt - $measured.Start).TotalSeconds; $measured.End = $script:stopAt }
                        elseif ($step.until) { $result.Events = "cap of $($step.minutes) min reached before $(Format-Until $step.until)" }
                        Complete-Measured $measured $machine $notes
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
                { $_ -in 'camera_jumps', 'vfx_burst' } {
                    $a = @{}; foreach ($p in $step.PSObject.Properties) { if ($p.Name -ne 'do') { $a[$p.Name] = $p.Value } }
                    $rep = Call-Agent $step.do $a
                    Say "  step ${n}: $($step.do) $(if ($rep.path) { 'over ' + @($rep.path).Count + ' lots' } else { "$($rep.count) effects" }) in the background"
                }
                'mode' {
                    $want = $script:ModeStates[[string]$step.mode]
                    if (-not $want) { throw "mode: '$($step.mode)' is not one of $($script:ModeStates.Keys -join ', ')" }
                    Call-Agent 'set_mode' @{ mode = $step.mode } | Out-Null
                    Say "  step ${n}: mode $($step.mode)"
                    Wait-Game "mode $($step.mode)" $LoadTimeoutMin {
                        try { $s = Call-Agent 'game_state' $null 5000; return ($s.sub_state -eq $want -and -not $s.modal) } catch { return $false }
                    } { Clear-Dialog }
                    if ($step.seconds) { Start-Sleep -Seconds ([double]$step.seconds) } # time in the mode before the next step
                }
                'stop_repros' {
                    $j = Call-Agent 'camera_jumps' @{ stop = $true }; $v = Call-Agent 'vfx_burst' @{ stop = $true }
                    $counts = 'jumps {0}, loops {1}, outfit changes {2}, Sims spawned {3} (removed {4}), effect rounds {5}, effects {6}, missing {7}' -f
                        $j.jumps, $j.loops, $j.outfitChanges, $j.spawned, $j.removed, $v.rounds, $v.started, $v.missing
                    $result.Events = (@($result.Events, $counts) | Where-Object { $_ }) -join '; '
                    Say "  step ${n}: repros stopped: $($result.Events)"
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
            if ($step.do -notin 'wait', 'profile') {
                $label = if ($step.do -eq 'mode') { "mode $($step.mode)" } else { $step.do }
                $script:commands.Add([pscustomobject]@{ label = $label; kind = 'step'; t0 = $stepT0; t1 = (Get-QpcMs) })
            }
        }
        if ($measured) { Complete-Measured $measured $machine $notes } # a profile step's window may outlast the steps
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
    $script:commands | ForEach-Object { $_ | ConvertTo-Json -Compress } | Set-Content -LiteralPath (Join-Path $dir 'commands.jsonl') -Encoding UTF8
    if ($measured) {
        # the frame recorder's session ends with the game; give it a moment, then end it
        if ($measured.Prof.Frames -and -not $measured.Prof.Frames.WaitForExit(30000)) { $measured.Prof.Frames.Kill(); $notes.Add('frame recorder still attached after the game closed; ended') }
        $machine | ForEach-Object { $_ | ConvertTo-Json -Compress } | Set-Content -LiteralPath "$($measured.Base)-machine.jsonl" -Encoding UTF8
        $crash = if (Test-Path (Join-Path $dir 'ApexRadiance_Crash.txt')) { 'crash report written' } elseif ($result.Closed -notin 'quit', 'window closed') { "game did not quit ($($result.Closed))" } else { $null }
        $result.M = Get-Measurement $measured.Base $measured.Seconds $script:commands $machine -NoInactiveIdle:(-not $InactiveIdle) -Crash $crash
        if ($result.M) { $result.Frames = $result.M.Frames; $result.Sim = $result.M.Sim }
    }
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
        $results | Select-Object Name, JobId, Variant, Ok, Closed, Frames, Sim, Events, Notes | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutDir 'results.json') -Encoding UTF8
        Say "results: $OutDir\results.md ($(@($results | Where-Object Ok).Count) of $($results.Count) loads passed)"
    }
}
# 0 all loads passed, 1 a load failed, 2 the run stopped early (see runner_lib.ps1 Get-RunExitCode)
exit (Get-RunExitCode $results $loads.Count)

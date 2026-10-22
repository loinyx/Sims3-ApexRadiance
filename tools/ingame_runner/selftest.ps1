# Offline checks of the in-game runner: no game, nothing installed, scratch files under %TEMP% only.
# Covers the TOML switches, the ABBA load order, the log pass rule, the frame statistics, every job file, and the
# mailbox client against an in-process stand-in for the game's agent (same layout, same magic).
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

# Frame statistics: 100 focused frames of 16 ms, one of 120 ms, 10 unfocused frames that do not count
$csv = Join-Path ([IO.Path]::GetTempPath()) ('apex-ingame-selftest-' + [Guid]::NewGuid().ToString('N') + '.csv')
$rows = @('epochMs,qpcMs,frameMs,workMs,fg') + (1..100 | ForEach-Object { "$_,$_,16,5,1" }) + @('101,101,120,5,1') + (1..10 | ForEach-Object { "$_,$_,33,5,0" })
try {
    Set-Content -LiteralPath $csv -Value $rows
    $f = Get-FrameStats $csv
    Check ($f.Frames -eq 101 -and $f.P50 -eq 16 -and $f.Max -eq 120) 'frame percentiles over focused frames'
    Check ([math]::Abs($f.Over50PerMin - (1 / (1720 / 60000))) -lt 0.1) 'long frames per minute'
} finally { Remove-Item -LiteralPath $csv -ErrorAction SilentlyContinue }

# Results page
$res = @([pscustomobject]@{ Name = 'x-1-off'; JobId = 'x'; Variant = 'off'; Ok = $true; Closed = 'quit'; Frames = $f; Sim = $null; Notes = @() },
         [pscustomobject]@{ Name = 'x-2-on'; JobId = 'x'; Variant = 'on'; Ok = $false; Closed = 'quit'; Frames = $null; Sim = $null; Notes = @('log: a|b') })
$md = Format-Results $res 'Self-test'
Check ($md -match '\| x-2-on \| FAIL \|' -and $md -match '## x: A/B' -and $md -match 'log: a/b') 'results page'

# Every shipped job: valid JSON, an id equal to its file name, a save, known steps, patterns that compile
$known = 'speed', 'wait', 'camera', 'camera_pan', 'save', 'reload'
foreach ($file in Get-ChildItem (Join-Path $PSScriptRoot 'jobs') -Filter *.json) {
    $j = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    Check ($j.id -eq $file.BaseName -and $j.save -and @(Get-JobLoads $j -AB).Count -ge 1) "$($file.Name): id, save, variants"
    Check (@(Get-JobSteps $j | Where-Object { $known -notcontains $_.do }).Count -eq 0) "$($file.Name): steps"
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
} finally { $client.Dispose(); [FakeAgent]::Stop() }

Write-Output "$($script:checks) checks, $($script:failures) failures"
if ($script:failures) { exit 1 }

<#
UI smoke run for Apex Launcher: starts the published exe in a dry run (APEX_SETUP_DRY_RUN=1, nothing is written to the
game or the drivers) against a game folder and drives every interactive control through UI Automation. After each step it
checks that the launcher still runs and that the player log has no new error other than the dry run's own refusals.
The dry run also keeps it portable: it does not install itself, add shortcuts or start with Windows.

  powershell -ExecutionPolicy Bypass -File installer\ui-smoke\Run-UiSmoke.ps1 -Exe installer\publish\ApexLauncher.exe `
      -Game "C:\Games\The Sims 3" [-FakeNv "NVIDIA GeForce RTX 4070|62124|0|"] [-FakeAmd "1|0"] [-Runs 3]

The language choice (language.txt) is put back as it was. Explorer windows the log folder opens are closed again.
Exit code: the number of failed steps.
#>
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$Game,
    [string]$FakeNv = $null,
    [string]$FakeAmd = $null,
    [int]$Runs = 3,
    [switch]$Quick, # (skips the language cycle)
    [switch]$Trace, # (prints each step and what it found)
    [string]$Shots = $null # (a folder: each notice and dialog state saved there as a PNG)
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SmokeWin {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
}
"@
$A = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
function Cond($prop, $val) { New-Object System.Windows.Automation.PropertyCondition($prop, $val) }
function And($a, $b) { New-Object System.Windows.Automation.AndCondition($a, $b) }

$appData = Join-Path $env:LOCALAPPDATA 'Apex Launcher'
$log = Join-Path $appData 'ApexLauncher.log'
$langFile = Join-Path $appData 'language.txt'
$langSaved = if (Test-Path $langFile) { [IO.File]::ReadAllText($langFile) } else { $null }
$script:failures = New-Object System.Collections.Generic.List[string]
$script:steps = 0

# the window as it is now, to $Shots\<name>.png (first run only)
function Shot([string]$name) {
    if (-not $Shots -or $script:run -ne 1) { return }
    Add-Type -AssemblyName System.Drawing
    New-Item -ItemType Directory -Force $Shots | Out-Null
    $r = New-Object SmokeWin+RECT; [void][SmokeWin]::GetWindowRect($script:hwnd, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][SmokeWin]::PrintWindow($script:hwnd, $dc, 2)
    $g.ReleaseHdc($dc); $g.Dispose(); $bmp.Save((Join-Path $Shots "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

function LogLength { if (Test-Path $log) { (Get-Item $log).Length } else { 0 } }

# a step: run it, then Setup must still run and the log must have nothing new but the dry run's refusals
function Step([string]$what, [scriptblock]$do) {
    $script:steps++
    if ($Trace) { Write-Output "step: $what" }
    # (the first step also checks what Setup logged while it started)
    $before = if ($null -ne $script:launchLog) { $script:launchLog } else { LogLength }
    $script:launchLog = $null
    try { & $do } catch { $script:failures.Add("${what}: automation error $($_.Exception.Message)") }
    Start-Sleep -Milliseconds 350
    if ($script:proc.HasExited) {
        $script:failures.Add("${what}: SETUP EXITED (code $($script:proc.ExitCode))")
        throw "Setup exited at: $what"
    }
    $after = LogLength
    if ($after -gt $before) {
        $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
        try { $fs.Position = $before; $new = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
        $entries = $new -split "(?m)^(?=\d{4}-\d\d-\d\dT)" | Where-Object { $_.Trim() }
        foreach ($e in $entries) {
            if ($e -match 'Dry run: (Setup changes nothing|the game is not started)|Test run: the launcher changes nothing') { continue }
            $script:failures.Add("${what}: LOG $($e.Trim() -replace '\s+', ' ' | ForEach-Object { $_.Substring(0, [Math]::Min(600, $_.Length)) })")
        }
    }
}

function Root { $A::FromHandle($script:hwnd) }
function ById([string]$id) { (Root).FindFirst($TS::Descendants, (Cond $A::AutomationIdProperty $id)) }
function ByName([string]$n) { (Root).FindFirst($TS::Descendants, (Cond $A::NameProperty $n)) }
function Invoke($el) { $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Toggle($el) { $el.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle() }
function Sel($el) { $el.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
function Nav([string]$idOrName) {
    $el = if ($idOrName -eq 'NavComponents') { ById 'NavComponents' } else { ByName $idOrName }
    if (-not $el) { throw "nav item $idOrName not found" }
    try { Sel $el } catch { Invoke $el }
    Start-Sleep -Milliseconds 600
}
# a screen that is no longer in the sidebar: opened from Settings' rows
function Open([string]$screen) {
    $id = @{ 'Game' = 'SettingsGame'; 'Repair or remove' = 'SettingsManage'; 'About and licences' = 'SettingsAbout' }[$screen]
    if (-not $id) { Nav $screen; return }
    Nav 'Settings'
    $b = ById $id
    if (-not $b) { throw "settings row $id not found" }
    Invoke $b
    Start-Sleep -Milliseconds 600
}
function Buttons { (Root).FindAll($TS::Descendants, (Cond $A::IsInvokePatternAvailableProperty $true)) }

# an open ContentDialog: Copy details once, then its primary choice (the dry run refuses it) or OK
function Dialogs([switch]$Links) {
    for ($i = 0; $i -lt 4; $i++) {
        $b = Buttons | Where-Object { $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button }
        $names = $b | ForEach-Object { $_.Current.Name }
        if ($Links) {
            $copy = Buttons | Where-Object { $_.Current.Name -eq 'Copy details' } | Select-Object -First 1
            if ($copy) { Invoke $copy; Start-Sleep -Milliseconds 300 }
            # (Open log folder is not pressed: it would open a window on the player's screen)
        }
        $pick = $null
        foreach ($n in 'Remove', 'Restore', 'Uninstall', 'Undo these changes', 'Continue', 'OK') {
            if ($names -contains $n -and ($names -contains 'Cancel' -or $n -in 'OK', 'Undo these changes')) { $pick = $b | Where-Object { $_.Current.Name -eq $n } | Select-Object -First 1; break }
        }
        if (-not $pick) { return }
        Invoke $pick
        Start-Sleep -Seconds 2
    }
}
function CloseExplorer {
    $w = [System.Windows.Automation.AutomationElement]::RootElement.FindAll($TS::Children, (Cond $A::ClassNameProperty 'CabinetWClass'))
    foreach ($x in $w) { if ($x.Current.Name -like '*Apex Radiance Setup*') { $x.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close() } }
}

function FrameLimitPicker([string]$label) {
    $cb = ById 'FpsCombo'
    if (-not $cb -or $cb.Current.IsOffscreen -and -not $cb.Current.IsEnabled) { Step "frame limit ($label): not shown" { }; return }
    $count = 0
    Step "frame limit ($label): open" {
        $cb.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 500
        $script:items = (ById 'FpsCombo').FindAll($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::ListItem)))
        (ById 'FpsCombo').GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()
    }
    $n = [Math]::Max($script:items.Count, 11)
    if ($Trace) { Write-Output "  frame limit items: $($script:items.Count)" }
    for ($k = 0; $k -lt $n; $k++) {
        Step "frame limit ($label): item $k" {
            $c = ById 'FpsCombo'
            $c.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 400
            $its = (ById 'FpsCombo').FindAll($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::ListItem)))
            if ($k -lt $its.Count) { Sel $its[$k] } else { (ById 'FpsCombo').GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse() }
            Start-Sleep -Milliseconds 500
        }
    }
    # Custom: the number box, with values in and out of range and an empty one
    Step "frame limit ($label): Custom" {
        $c = ById 'FpsCombo'
        $c.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 400
        $its = (ById 'FpsCombo').FindAll($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::ListItem)))
        if ($its.Count -gt 0) { Sel $its[$its.Count - 1] }
        Start-Sleep -Milliseconds 500
    }
    foreach ($v in '15', '20', '75', '1000', '5000', '') {
        Step "frame limit ($label): custom '$v'" {
            $box = ById 'FpsCustom'
            if (-not $box) { throw "the Custom number box is not there" }
            $edit = $box.FindFirst($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::Edit)))
            $target = if ($edit) { $edit } else { $box }
            if ($v -ne '') {
                try { $box.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern).SetValue([double]$v) }
                catch { $target.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($v) }
            } else { $target.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('') }
            (ById 'FpsCombo').SetFocus()
            Start-Sleep -Milliseconds 500
        }
    }
    Step "frame limit ($label): back to Keep" {
        $c = ById 'FpsCombo'
        $c.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand(); Start-Sleep -Milliseconds 400
        $its = (ById 'FpsCombo').FindAll($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::ListItem)))
        if ($its.Count -gt 0) { Sel $its[0] }
        Start-Sleep -Milliseconds 500
    }
}

function OneRun([int]$run) {
    $script:run = $run
    $env:APEX_SETUP_DRY_RUN = '1'
    $env:APEX_SETUP_FAKE_NV = $FakeNv
    $env:APEX_SETUP_FAKE_AMD = $FakeAmd
    $script:launchLog = LogLength
    $script:proc = Start-Process $Exe -ArgumentList "`"$Game`"" -PassThru
    $script:hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 60 -and $script:hwnd -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 500; $script:proc.Refresh(); $script:hwnd = $script:proc.MainWindowHandle }
    Start-Sleep -Seconds 8 # (the release check and the drivers' first read)
    try {
        Step "run ${run}: start" { }
        # the launcher's own screens: each one, Play (refused by the dry run), a mod search, Health's Check now and its tabs
        foreach ($n in 'Home', 'Game settings', 'Mods', 'Saves', 'Health', "What's new", 'Settings') {
            Nav $n
            Step "screen $n" { Shot ("screen-" + ($n -replace '[^A-Za-z]', '')) }
        }
        Nav 'Home'
        Step "play" { $p = ById 'PlayButton'; if ($p -and $p.Current.IsEnabled) { Invoke $p; Start-Sleep -Seconds 2; Dialogs } }
        Nav 'Mods'
        Step "mods search" { $box = (ById 'ModSearch').FindFirst($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::Edit))); if ($box) { $box.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('Packages'); Start-Sleep -Seconds 1; $box.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('') } }
        Step "mods grid" { $g = ByName 'Grid'; if ($g) { Toggle $g; Start-Sleep -Seconds 2; Shot 'mods-grid'; Toggle (ByName 'List') } }
        Nav 'Health'
        Step "health check now" { Invoke (ById 'HealthCheckNow'); Start-Sleep -Seconds 2 }
        foreach ($t in 'HealthMods', 'HealthStability', 'HealthSaves') { Step "health page $t" { Invoke (ById $t); Start-Sleep -Milliseconds 800; Invoke (ById 'HealthBack'); Start-Sleep -Milliseconds 800 } }
        # Game: each listed game, then the first again
        Open 'Game'
        $games = (ById 'GameList').FindAll($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::ListItem)))
        for ($g = $games.Count - 1; $g -ge 0; $g--) { Step "game $g" { Sel $games[$g]; $games[$g].SetFocus() } }
        # Components: every switch three times (on, off, on) and once more back, the renderer cards
        Nav 'NavComponents'
        Step "components" { }
        $toggles = (Root).FindAll($TS::Descendants, (Cond $A::IsTogglePatternAvailableProperty $true)) | Where-Object { $_.Current.AutomationId -like '*Toggle' }
        if ($Trace) { Write-Output "  switches: $(@($toggles | ForEach-Object { $_.Current.AutomationId }) -join ', ')" }
        foreach ($t in $toggles) {
            $id = $t.Current.AutomationId
            for ($k = 0; $k -lt 4; $k++) { Step "toggle $id #$k" { $x = ById $id; if ($x.Current.IsEnabled) { Toggle $x } } }
            Step "footer after $id" { [void](ById 'NextButton').Current.Name }
        }
        $radios = (Root).FindAll($TS::Descendants, (Cond $A::ControlTypeProperty ([System.Windows.Automation.ControlType]::RadioButton)))
        $first = $null
        foreach ($r in $radios) { if (-not $first) { try { if ($r.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) { $first = $r.Current.Name } } catch { } } }
        foreach ($r in $radios) { $nm = $r.Current.Name; Step "renderer '$($nm.Split([char]10)[0])'" { Sel ((Root).FindFirst($TS::Descendants, (Cond $A::NameProperty $nm))) } }
        if ($first) { Step "renderer back" { Sel ((Root).FindFirst($TS::Descendants, (Cond $A::NameProperty $first))) } }
        # the frame limit with frame generation off, then turning on (pending), then back
        FrameLimitPicker 'frame generation off'
        foreach ($fg in 'SmoothToggle', 'AfmfToggle') {
            $x = ById $fg
            if ($x -and $x.Current.IsEnabled -and -not $x.Current.IsOffscreen) {
                Step "$fg on" { Toggle (ById $fg) }
                FrameLimitPicker "$fg pending"
                Step "$fg back" { Toggle (ById $fg) }
            }
        }
        # Apply in the dry run: a pending switch (the driver change's dry run, then Done), then a component change (the error dialog)
        $x = ById 'SmoothToggle'; if (-not ($x -and $x.Current.IsEnabled)) { $x = ById 'AfmfToggle' }
        if ($x -and $x.Current.IsEnabled) {
            $xid = $x.Current.AutomationId
            Step "apply a driver change: pending" { Toggle (ById $xid); Start-Sleep -Milliseconds 600; Shot 'components-pending' }
            Step "apply a driver change: Apply" { Invoke (ById 'NextButton'); Start-Sleep -Seconds 3; Dialogs }
            Step "done screen" { [void](ById 'DoneTitle'); Shot 'done' }
            Nav 'NavComponents'
        }
        Step "apply a component change" {
            $r = ById 'RestartToggle'; if ($r -and $r.Current.IsEnabled) { Toggle $r; Start-Sleep -Milliseconds 400; Invoke (ById 'NextButton'); Start-Sleep -Seconds 4; Shot 'error-dialog'; Dialogs -Links }
        }
        Nav 'NavComponents'
        Step "back button" { $b = ById 'BackButton'; if ($b -and -not $b.Current.IsOffscreen) { Invoke $b } }
        # Repair or remove: every action (each refused by the dry run), the restore points
        Open 'Repair or remove'
        Step "manage" { }
        $acts = Buttons | Where-Object { $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button -and $_.Current.Name -match '^(Repair|Remove|Restore|Uninstall) ' }
        $labels = @($acts | ForEach-Object { $_.Current.Name })
        if ($Trace) { Write-Output "  manage actions: $($labels -join ', ')" }
        for ($k = 0; $k -lt $labels.Count; $k++) {
            Step "manage action $k ($($labels[$k]))" {
                Open 'Repair or remove'
                $all = @(Buttons | Where-Object { $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button -and $_.Current.Name -match '^(Repair|Remove|Restore|Uninstall) ' })
                if ($k -lt $all.Count) { Invoke $all[$k]; Start-Sleep -Seconds 3; if ($k -le 1) { Shot "manage-confirm-$k" }; Dialogs -Links:($k -eq 0); Start-Sleep -Milliseconds 800; Shot "manage-result-$k" }
                $copy = Buttons | Where-Object { $_.Current.Name -eq 'Copy details' } | Select-Object -First 1
                if ($copy) { Invoke $copy }
            }
        }
        # About and licences: each licence (the GitHub links open a browser: left alone)
        Open 'About and licences'
        $reads = @(Buttons | Where-Object { $_.Current.Name -eq 'Read' })
        for ($k = 0; $k -lt $reads.Count; $k++) { Step "licence $k" { (@(Buttons | Where-Object { $_.Current.Name -eq 'Read' }))[$k] | ForEach-Object { Invoke $_ } } }
        # every language on the Components step, then English
        if (-not $Quick) {
            Nav 'NavComponents'
            $langs = 'Português', 'Español', 'Français', 'Deutsch', 'Italiano', 'Nederlands', 'Polski', 'Русский', 'Čeština', 'Magyar', 'Ελληνικά', 'Dansk', 'Svenska', 'Norsk', 'Suomi', '日本語', '한국어', '简体中文', '繁體中文', 'ไทย', 'English'
            foreach ($l in $langs) {
                Step "language $l" {
                    # (the menu opens as a popup that UI Automation sometimes lists late: asked again, the menu opened again)
                    $item = $null
                    for ($try = 0; $try -lt 4 -and -not $item; $try++) {
                        if ($try -eq 0 -or $try -eq 2) { Invoke (ById 'LangButton') }
                        Start-Sleep -Milliseconds (700 + 500 * $try)
                        $item = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst($TS::Descendants, (And (Cond $A::NameProperty $l) (Cond $A::ProcessIdProperty $script:proc.Id)))
                    }
                    if (-not $item) { throw "language item $l not found" }
                    try { Invoke $item } catch { Toggle $item }
                    Start-Sleep -Milliseconds 900
                    Nav 'NavComponents'
                }
            }
        }
        # narrow and wide
        Step "narrow" { [void][SmokeWin]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 80, 60, 640, 900, 0x14); Start-Sleep -Seconds 1; Open 'Repair or remove'; Shot 'manage-narrow' }
        Step "wide" { [void][SmokeWin]::SetWindowPos($script:hwnd, [IntPtr]::Zero, 40, 40, 1600, 1000, 0x14); Start-Sleep -Seconds 1 }
        Step "run ${run}: end" { }
    }
    finally {
        if (-not $script:proc.HasExited) { Stop-Process -Id $script:proc.Id -Force }
        # (no launcher left behind: the notification area icon and the single-instance lock go with it; Windows can take
        # more than a second to end it after the kill, so wait for the exit itself)
        if (-not $script:proc.WaitForExit(15000)) { $script:failures.Add("run ${run}: the launcher was still running after it was closed") }
        CloseExplorer
    }
}

try {
    for ($r = 1; $r -le $Runs; $r++) {
        try { OneRun $r } catch { Write-Output "run $r stopped: $($_.Exception.Message)" }
    }
}
finally {
    if ($null -eq $langSaved) { if (Test-Path $langFile) { Remove-Item $langFile } } else { [IO.File]::WriteAllText($langFile, $langSaved) }
}
Write-Output "steps: $script:steps, failures: $($script:failures.Count)"
$script:failures | ForEach-Object { Write-Output "FAIL $_" }
exit $script:failures.Count

# Drives the real launcher through the Mods screen's features on a sandbox: a temporary Documents folder holding copies
# of a few packages and a temporary launcher data folder, so the player's mods, notes and settings are never touched.
# Each step checks the result on disk, not only on screen, and saves a window capture.
#   powershell -File installer\ui-smoke\Run-ModsHarness.ps1 -Exe <ApexLauncher.exe> -Game <game folder> -Packages <folder of .package files> [-Out <folder>]
param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$Game, [Parameter(Mandatory)][string]$Packages, [string]$Out = "$env:TEMP\apex-mods-harness")

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class HW { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
public struct R { public int L, T, Rt, B; } }
"@
$A = [System.Windows.Automation.AutomationElement]; $TS = [System.Windows.Automation.TreeScope]
if (Get-Process TS3W, TS3 -ErrorAction SilentlyContinue) { throw "The Sims 3 is running: the harness does not start while the game is open." }

# the sandbox
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
$docs = Join-Path $Out 'Docs'; $data = Join-Path $Out 'Data'; $shots = Join-Path $Out 'shots'
$pk = Join-Path $docs 'Mods\Packages'
New-Item -ItemType Directory -Force (Join-Path $pk 'Set'), $data, $shots | Out-Null
Set-Content (Join-Path $docs 'Mods\Resource.cfg') "Priority 500`r`nPackedFile Packages/*.package`r`nPackedFile Packages/*/*.package`r`nPackedFile Packages/*/*/*.package" -Encoding ascii
$src = Get-ChildItem $Packages -Filter *.package | Sort-Object Length | Select-Object -First 6
if ($src.Count -lt 4) { throw "Need at least four packages in $Packages" }
$src | ForEach-Object { Copy-Item $_.FullName (Join-Path $pk 'Set') }
$names = $src | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.Name) }

$env:APEX_SETUP_DOCUMENTS = $docs; $env:APEX_LAUNCHER_LOCALAPPDATA = $data; $env:APEX_LAUNCHER_PORTABLE = '1'; $env:APEX_LAUNCHER_LANG = 'en'
Remove-Item Env:APEX_SETUP_DRY_RUN -ErrorAction SilentlyContinue
Get-Process ApexLauncher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$p = Start-Process $Exe -ArgumentList "`"$Game`"" -PassThru
Start-Sleep 10; $p.Refresh(); $h = $p.MainWindowHandle
if ($h -eq 0) { throw "No window" }
$root = $A::FromHandle($h)
$fails = 0; $step = 0
function Snap($name) {
  [HW]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 700
  $r = New-Object HW+R; [HW]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Rt - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [HW]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
  $bmp.Save((Join-Path $shots ("{0:00}-{1}.png" -f $script:step, $name))); $g.Dispose(); $bmp.Dispose()
}
function Find($name, $scope = $root) { $scope.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $name))) }
function Wait-Find($name, $secs = 8) { $t = [DateTime]::Now.AddSeconds($secs); do { $e = Find $name; if (-not $e) { $e = $A::RootElement.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $name))) }; if ($e) { return $e }; Start-Sleep -Milliseconds 300 } while ([DateTime]::Now -lt $t); return $null }
# the first element of that name that can be pressed (a button, a menu item), not a text that says the same
function Wait-Pressable($name, $secs = 8) {
  $cond = New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $name)
  $t = [DateTime]::Now.AddSeconds($secs)
  do {
    foreach ($scope in @($root, $A::RootElement)) {
      foreach ($e in $scope.FindAll($TS::Descendants, $cond)) {
        if ($e.Current.IsEnabled -and ($e.GetSupportedPatterns() | Where-Object { $_.ProgrammaticName -match 'Invoke|SelectionItem|Toggle' })) { return $e }
      }
    }
    Start-Sleep -Milliseconds 300
  } while ([DateTime]::Now -lt $t)
  return $null }
function Click($name) { $e = Wait-Pressable $name; if (-not $e) { throw "not found: $name" }
  $pats = $e.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName }
  if ($pats -match 'InvokePattern') { $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
  elseif ($pats -match 'SelectionItemPattern') { $e.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
  else { $e.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle() }
  Start-Sleep -Milliseconds 900 }
# a dialog's own button by its template part (PrimaryButton, CloseButton), never the window's caption Close
function Dialog($part, $secs = 10) {
  $cond = New-Object System.Windows.Automation.PropertyCondition($A::AutomationIdProperty, $part)
  $t = [DateTime]::Now.AddSeconds($secs)
  do {
    foreach ($scope in @($root, $A::RootElement)) {
      foreach ($e in $scope.FindAll($TS::Descendants, $cond)) {
        if ($e.Current.IsEnabled -and -not $e.Current.IsOffscreen) { $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Milliseconds 900; return }
      }
    }
    Start-Sleep -Milliseconds 300
  } while ([DateTime]::Now -lt $t)
  throw "no dialog button $part" }
function Pick($name, [switch]$Add) { $e = Wait-Find $name; if (-not $e) { throw "not found: $name" }
  $sp = $e.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern); if ($Add) { $sp.AddToSelection() } else { $sp.Select() }; Start-Sleep -Milliseconds 900 }
function Type-Into($name, $text) { $e = Wait-Find $name; if (-not $e) { throw "not found: $name" }
  $isEdit = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
  # (a text box of that name first: a tab can have the same name, as Notes has)
  $named = $root.FindFirst($TS::Descendants, (New-Object System.Windows.Automation.AndCondition($isEdit, (New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $name)))))
  $edit = if ($named) { $named } elseif ($e.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) { $e } else { $e.FindFirst($TS::Descendants, $isEdit) }
  $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($text); Start-Sleep -Milliseconds 400 }
function Step($what, [scriptblock]$do, [scriptblock]$ok) {
  $script:step++
  try { & $do; Start-Sleep -Milliseconds 800; $good = & $ok } catch { $good = $false; Write-Output "  error: $($_.Exception.Message)" }
  Snap ($what -replace '[^A-Za-z]', '')
  if ($good) { Write-Output ("ok   {0}" -f $what) } else { Write-Output ("FAIL {0}" -f $what); $script:fails++ }
}
$set = Join-Path $pk 'Set'
$off = Join-Path $docs 'Mods\Disabled (Apex Launcher)\Packages\Set'
$first = $names[0]; $second = $names[1]; $third = $names[2]

Step "open Mods and the Set folder" { Click 'Mods'; Start-Sleep 3; Pick "Set $($src.Count)" } { (Find $first) -ne $null }
Step "turn a mod off from its details" { Pick $first; Click 'Turn off' } { (Test-Path (Join-Path $off "$first.package")) -and -not (Test-Path (Join-Path $set "$first.package")) }
Step "turn it back on" { Pick $first; Click 'Turn on' } { (Test-Path (Join-Path $set "$first.package")) -and -not (Test-Path (Join-Path $off "$first.package")) }
Step "make a tag and give it to a mod" { Pick $second; Click 'Add tag'; Click 'New tag…'; Type-Into 'Tag name' 'Harness'; Dialog 'PrimaryButton' } { (Get-Content (Join-Path $data 'Apex Launcher\mod-notes.json') -Raw) -match '"Harness"' }
Step "notes are kept" { Pick $second; Click 'Notes'; Type-Into 'Notes' 'from the harness'; Pick $third; Pick $second; Click 'Overview' } { (Get-Content (Join-Path $data 'Apex Launcher\mod-notes.json') -Raw) -match 'from the harness' }
Step "rename a mod in place" { Pick $third; Click 'Rename'; Type-Into 'New name' 'Renamed by harness'; [System.Windows.Forms.SendKeys]::SendWait('{ENTER}') } { (Get-ChildItem $set -Filter '*Renamed by harness.package').Count -eq 1 }
Step "undo the rename" { Click 'Undo' } { Test-Path (Join-Path $set "$third.package") }
Step "merge three mods, verified, with a log" {
  Pick $names[3]; Pick $names[4] -Add; Pick $names[5] -Add
  Click 'Merge into one package…'; Start-Sleep 2; Dialog 'PrimaryButton'; Start-Sleep 6; Dialog 'CloseButton' } {
  (Get-ChildItem $set -Filter '*merged*.package').Count -eq 1 -and (Get-ChildItem (Join-Path $docs 'Merged (Apex Launcher)') -Recurse -Filter merge.json).Count -eq 1 }
Step "split the merge back" {
  $m = (Get-ChildItem $set -Filter '*merged*.package')[0].BaseName; Pick $m; Click 'Split it back into its 3 mods'; Dialog 'PrimaryButton'; Start-Sleep 4 } {
  (Get-ChildItem $set -Filter '*merged*.package').Count -eq 0 -and (Test-Path (Join-Path $set "$($names[4]).package")) }
Step "start a problem-mod hunt" { Click 'Find a problem mod'; Dialog 'PrimaryButton'; Start-Sleep 3 } { (Get-ChildItem $off -Filter *.package -ErrorAction SilentlyContinue).Count -ge 2 }
Step "stop it: every mod back" { Click 'Stop and put every mod back'; Start-Sleep 3 } { (Get-ChildItem $off -Filter *.package -ErrorAction SilentlyContinue).Count -eq 0 -and (Get-ChildItem $set -Filter *.package).Count -eq $src.Count }
Step "make a folder" { Click 'New folder'; Type-Into 'New folder' 'Harness folder'; Dialog 'PrimaryButton' } { Test-Path (Join-Path $pk 'Set\Harness folder') }
$drop = Join-Path $Out 'drop'; New-Item -ItemType Directory -Force $drop | Out-Null
Copy-Item (Get-ChildItem $Packages -Filter *.package | Sort-Object Length | Select-Object -Skip 6 -First 1).FullName (Join-Path $drop 'Opened with.package') # (a mod not installed yet)
Set-Content (Join-Path $drop 'fake.package') 'not a package at all, only text in a file named like one, long enough for a header ........................' -Encoding ascii
Step "Open with: a second launcher hands two files over, only the real one is installed" {
  Start-Process $Exe -ArgumentList "`"$(Join-Path $drop 'Opened with.package')`" `"$(Join-Path $drop 'fake.package')`""; Start-Sleep 6
  Dialog 'PrimaryButton'; Start-Sleep 4; Dialog 'CloseButton' } {
  (Get-ChildItem $docs -Recurse -Filter 'Opened with.package').Count -eq 1 -and (Get-ChildItem $docs -Recurse -Filter 'fake.package').Count -eq 0 }
Step "remove a mod to the Recycle Bin, confirmed" { Pick ("Set {0}" -f (Get-ChildItem $set -Recurse -Filter *.package).Count); Pick $second; Click 'Remove to the Recycle Bin'; Dialog 'PrimaryButton'; Start-Sleep 2 } { -not (Test-Path (Join-Path $set "$second.package")) }

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Output ("steps: {0}, failures: {1}, captures: {2}" -f $step, $fails, $shots)
exit $fails

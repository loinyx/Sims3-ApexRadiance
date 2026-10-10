param(
    [Parameter(Mandatory=$true)][string]$GameBin,
    [string]$GhidraRoot = '',
    [switch]$Analyze,
    [switch]$Export
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $repo 'outputs/research'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$files = foreach ($name in @('TS3W.exe','TS3.exe','Shaders_Win32.precomp','ApexRadiance.asi','d3d9.dll','wininet.dll')) {
    $file = Join-Path $GameBin $name
    if (Test-Path -LiteralPath $file) {
        $item = Get-Item -LiteralPath $file
        $entry = [ordered]@{name=$name; path=$item.FullName; bytes=$item.Length; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        if ($name -in @('TS3W.exe','TS3.exe')) {
            $stream = [IO.File]::OpenRead($file)
            $reader = [IO.BinaryReader]::new($stream)
            try {
                if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Invalid DOS header.' }
                $stream.Position = 0x3C
                $offset = $reader.ReadInt32()
                if ($offset -lt 0 -or $offset + 12 -gt $stream.Length) { throw 'Invalid PE offset.' }
                $stream.Position = $offset
                if ($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE signature.' }
                $entry.machine = ('0x{0:X4}' -f $reader.ReadUInt16())
                $null = $reader.ReadUInt16()
                $entry.peTimestamp = ('0x{0:X8}' -f $reader.ReadUInt32())
            } finally { $reader.Dispose() }
        }
        $entry
    }
}
$files | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'inventory.json') -Encoding utf8
if ($Analyze -or $Export) {
    if (-not $GhidraRoot) { throw 'Supply the official extracted Ghidra directory with -GhidraRoot.' }
    $steam = Join-Path $GameBin 'TS3W.exe'
    if (-not (Test-Path -LiteralPath $steam)) { throw 'This analysis recipe requires the local Steam TS3W.exe; EA decrypted dumps are excluded.' }
    $steamEntry = $files | Where-Object { $_.name -eq 'TS3W.exe' }
    if ($steamEntry.peTimestamp -ne '0x52DEC247') { throw 'The selected address export is only defined for Steam 1.67.2.024037.' }
    $projects = Join-Path $out 'ghidra-projects'
    New-Item -ItemType Directory -Path $projects -Force | Out-Null
    $name = 'Steam-' + (Get-FileHash -LiteralPath $steam -Algorithm SHA256).Hash.Substring(0,12)
    $headless = Join-Path $GhidraRoot 'support/analyzeHeadless.bat'
    if (-not (Test-Path $headless)) { throw 'Ghidra analyzeHeadless was not found.' }
    if ($Analyze) {
        if (Test-Path (Join-Path $projects ($name + '.gpr'))) { throw 'This executable is already imported; open the existing project instead of overwriting it.' }
        & $headless $projects $name -import $steam -max-cpu 4 -analysisTimeoutPerFile 600 -log (Join-Path $out 'analysis.log')
        if ($LASTEXITCODE -ne 0) { throw 'Ghidra analysis failed; inspect outputs/research/analysis.log.' }
    }
    if ($Export) {
        if (-not (Test-Path (Join-Path $projects ($name + '.gpr')))) { throw 'Run -Analyze before exporting.' }
        & $headless $projects $name -process TS3W.exe -noanalysis -readOnly -scriptPath (Join-Path $PSScriptRoot 'ghidra') -postScript ApexResearchExport.java (Join-Path $out 'decompile') -log (Join-Path $out 'export.log')
        if ($LASTEXITCODE -ne 0) { throw 'Ghidra export failed; inspect outputs/research/export.log.' }
    }
}
Write-Output "Private inventory: $out/inventory.json"

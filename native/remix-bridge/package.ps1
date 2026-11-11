# Assembles the player package after build.cmd: out\package\ (to unzip into Game\Bin) and out\Apex-Radiance-RTX-Remix-bridge.zip.
# The server renders through stock DXVK x64; its d3d9.dll comes from the pinned DXVK release and is checked by hash.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'out'
$src = Join-Path $here 'src'
$dxvkVer = '3.1.1'
$dxvkTarSha = '40565B4A724AADC4433FA4E010B4B23916D9B1F1BAEEE64E17186DB94F54E608'
$dxvkDllSha = 'C126316F7478EAFF1AD8087702EDA5930F2400187B9C8B3592219C34F6632780'

foreach ($f in 'd3d9.dll', '.trex\NvRemixBridge.exe') {
    if (-not (Test-Path (Join-Path $out $f))) { throw "missing out\${f}: run build.cmd first" }
}

$dxvkDir = Join-Path $src 'dxvk'
$tar = Join-Path $dxvkDir "dxvk-$dxvkVer.tar.gz"
New-Item -ItemType Directory -Force $dxvkDir | Out-Null
if (-not (Test-Path $tar)) {
    Invoke-WebRequest "https://github.com/doitsujin/dxvk/releases/download/v$dxvkVer/dxvk-$dxvkVer.tar.gz" -OutFile $tar -UseBasicParsing
}
if ((Get-FileHash $tar -Algorithm SHA256).Hash -ne $dxvkTarSha) { Remove-Item $tar; throw "DXVK $dxvkVer archive hash mismatch" }
tar -xzf $tar -C $dxvkDir "dxvk-$dxvkVer/x64/d3d9.dll"
$dxvkDll = Join-Path $dxvkDir "dxvk-$dxvkVer\x64\d3d9.dll"
if ((Get-FileHash $dxvkDll -Algorithm SHA256).Hash -ne $dxvkDllSha) { throw "DXVK x64 d3d9.dll hash mismatch" }

$pkg = Join-Path $out 'package'
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
$trex = Join-Path $pkg '.trex'
New-Item -ItemType Directory -Force $trex | Out-Null
Copy-Item (Join-Path $out 'd3d9.dll') $pkg
Copy-Item (Join-Path $out '.trex\NvRemixBridge.exe') $trex
Copy-Item $dxvkDll (Join-Path $trex 'd3d9vk_x64.dll')
# the client reads .trex\bridge.conf, the server its own copy, DXVK the dxvk.conf in Bin (its working folder)
Copy-Item (Join-Path $here 'conf\bridge.conf') $pkg
Copy-Item (Join-Path $here 'conf\bridge.conf') $trex
Copy-Item (Join-Path $here 'conf\dxvk.conf') $pkg
# one text file: the install steps, then the licence texts the MIT and zlib terms ask to ship with the binaries
$text = @(Get-Content -Raw -Encoding UTF8 (Join-Path $here 'PLAYER-README.txt'))
foreach ($l in @(@('RTX Remix bridge', (Join-Path $src 'bridge\LICENSE-MIT')),
                 @('RTX Remix bridge third-party notices', (Join-Path $src 'bridge\ThirdPartyLicenses.txt')),
                 @('Microsoft Detours', (Join-Path $src 'submodules\Detours\LICENSE.md')),
                 @('DXVK', (Join-Path $here 'licenses\DXVK-LICENSE.txt')))) {
    $text += "`r`n`r`n==== Licence: $($l[0]) ====`r`n`r`n" + (Get-Content -Raw -Encoding UTF8 $l[1])
}
Set-Content -NoNewline -Encoding utf8 (Join-Path $pkg 'Apex Radiance RTX Remix bridge.txt') ($text -join '')

$zip = Join-Path $out 'Apex-Radiance-RTX-Remix-bridge.zip'
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
# entries named with '/' (Windows PowerShell 5.1's CreateFromDirectory writes a backslash, which some unzippers keep in the name)
$archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    foreach ($f in Get-ChildItem -Recurse -File -Force $pkg) {
        $name = $f.FullName.Substring($pkg.Length + 1).Replace([char]92, [char]47)
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name, 'Optimal') | Out-Null
    }
} finally { $archive.Dispose() }
"package: $pkg"
"zip: $zip ($((Get-Item $zip).Length) bytes)"

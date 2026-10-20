param()
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$header=Join-Path $repo 'apex_version.h'
$text=Get-Content -LiteralPath $header -Raw
$base=@('MAJOR','MINOR','PATCH') | ForEach-Object {[regex]::Match($text,"#define APEX_VERSION_$_ (\d+)").Groups[1].Value}
$revision=[int][regex]::Match($text,'#define APEX_PUBLIC_TEST_BUILD (\d+)').Groups[1].Value
$folder=Join-Path $repo 'outputs/test-builds'
New-Item -ItemType Directory -Path $folder -Force | Out-Null
do {
 $version=($base -join '.')+"-test.$revision"
 $target=Join-Path $folder "ApexRadiance-$version.asi"
 if(!(Test-Path -LiteralPath $target)){break}
 $revision++
} while($true)
$text=[regex]::Replace($text,'#define APEX_PUBLIC_TEST_BUILD \d+',"#define APEX_PUBLIC_TEST_BUILD $revision")
Set-Content -LiteralPath $header -Value $text.TrimEnd() -Encoding utf8
& (Join-Path $PSScriptRoot 'build.ps1') -Flavor Public -MemoryTest
Copy-Item -LiteralPath (Join-Path $repo 'Public/ApexRadiance.asi') -Destination $target
Get-FileHash -LiteralPath $target -Algorithm SHA256
Write-Output $target

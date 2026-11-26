param([string]$OutDir=(Join-Path $env:TEMP 'ApexOopHelperChecks'))
# Out-of-process helper checks (check.cpp): an x86 client (the game's side) against the x64 host part of ts3wrap.exe.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs){throw 'No installed MSVC toolchain found'}
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$check=Join-Path $PSScriptRoot 'check.cpp'
$host64=Join-Path $repo 'native/ts3wrap/oop_host.cpp'
$plugin=Join-Path $PSScriptRoot 'echo_plugin.cpp'
$common='/nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /DNOMINMAX /DUNICODE /D_UNICODE'
Push-Location -LiteralPath $OutDir
try {
 & cmd.exe /d /c ('call "' + $dev + '" -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe ' + $common + ' /DOOP_TEST_HOST /Feoop_host.exe "' + $check + '" "' + $host64 + '"')
 if($LASTEXITCODE -ne 0){throw 'x64 host compilation failed'}
 # the sample plugin next to the host: twice with its own tag, once reporting another ABI
 foreach($p in @(@('oop_echo1',''),@('oop_echo2','/DPLUGIN_TAG=0x32484345u'),@('oop_badabi','/DPLUGIN_ABI=0x00020000u'))){
  & cmd.exe /d /c ('call "' + $dev + '" -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe ' + $common + ' ' + $p[1] + ' /LD /Fe' + $p[0] + '.dll "' + $plugin + '"')
  if($LASTEXITCODE -ne 0){throw 'x64 plugin compilation failed'}
 }
 & cmd.exe /d /c ('call "' + $dev + '" -no_logo -arch=x86 -host_arch=x64 >nul && cl.exe ' + $common + ' /Feoop_client.exe "' + $check + '"')
 if($LASTEXITCODE -ne 0){throw 'x86 client compilation failed'}
 & '.\oop_client.exe' (Join-Path $OutDir 'oop_host.exe')
 if($LASTEXITCODE -ne 0){throw 'Out-of-process helper checks failed'}
} finally {Pop-Location}

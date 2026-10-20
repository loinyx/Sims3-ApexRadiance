param([Parameter(Mandatory=$true)][string]$OutDir,[Parameter(Mandatory=$true)][string]$Python)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
& $Python (Join-Path $PSScriptRoot 'extract.py') (Join-Path $OutDir 'notice_under_test.inc')
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
$tables=@(Get-ChildItem (Join-Path $repo 'i18n') -Filter '*.cpp' | ForEach-Object FullName)
$includeDirs=@($repo, (Join-Path $repo 'ui'), (Join-Path $repo 'framework'), $OutDir, (Join-Path $repo 'vcpkg_installed/x86-windows-static/include'))
$includes=@($includeDirs | ForEach-Object { '/I'+$_ })
Push-Location $OutDir
try {
 & cl.exe /nologo /std:c++20 /utf-8 /EHsc /O2 /MT /DAPEX_NO_DEV_TOOLS /DAPEX_MEMORY_TEST @includes /Fenotice_checks.exe (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $OutDir 'widgets_recorded.cpp') (Join-Path $repo 'ui/violet_theme.cpp') (Join-Path $repo 'ui/icons.cpp') (Join-Path $repo 'ui/i18n.cpp') @tables /link (Join-Path $repo 'vcpkg_installed/x86-windows-static/lib/imgui.lib') user32.lib imm32.lib gdi32.lib
 if($LASTEXITCODE -ne 0){throw 'Notice compilation failed'}
 & '.\notice_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Notice checks failed'}
 & cl.exe /nologo /std:c++20 /utf-8 /EHsc /O2 /MT @includes /Fei18n_checks.exe (Join-Path $repo 'tools/i18n_check/i18n_check.cpp') (Join-Path $repo 'ui/i18n.cpp') @tables /link user32.lib
 if($LASTEXITCODE -ne 0){throw 'Translation compilation failed'}
 Push-Location $repo
 try { & (Join-Path $OutDir 'i18n_checks.exe'); if($LASTEXITCODE -ne 0){throw 'Translation checks failed'} } finally{Pop-Location}
} finally{Pop-Location}

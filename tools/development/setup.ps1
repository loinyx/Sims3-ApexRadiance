param([string]$VcpkgRoot = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $repo 'vcpkg' }
$baseline = (Get-Content (Join-Path $repo 'vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Install Visual Studio 2022 C++ Build Tools (v143) and Windows SDK first.' }
$installation = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC v143 x86/x64 components are missing.' }
if (-not (Test-Path $VcpkgRoot)) {
    & git clone --no-checkout https://github.com/microsoft/vcpkg.git $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg clone failed.' }
    & git -C $VcpkgRoot checkout --detach $baseline
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg baseline checkout failed.' }
}
$head = & git -C $VcpkgRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $baseline) { throw "Expected vcpkg checkout $baseline; use a separate checkout at that revision." }
if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed.' }
}
Push-Location $repo
try {
    & (Join-Path $VcpkgRoot 'vcpkg.exe') install --triplet x86-windows-static --disable-metrics
    if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed.' }
} finally { Pop-Location }


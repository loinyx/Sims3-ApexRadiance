param([ValidateSet('Public','Developer')][string]$Flavor = 'Public')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio 2022 C++ Build Tools are required.' }
$msbuild = Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe'
$dependencies = Join-Path $repo 'vcpkg_installed/x86-windows-static'
foreach ($required in @('include/imgui.h','include/toml++/toml.hpp','lib/imgui.lib','lib/detours.lib')) {
    if (-not (Test-Path (Join-Path $dependencies $required))) { throw "Missing $required; run tools/development/setup.ps1." }
}
$relativeOutput = if ($Flavor -eq 'Public') { 'Public' } else { 'Release' }
$output = Join-Path $repo $relativeOutput
$intermediate = Join-Path $repo "obj/$Flavor"
$argsList = @((Join-Path $repo 'ApexRadiance.sln'), '/m', '/p:Configuration=Release', '/p:Platform=x86', '/p:PlatformToolset=v143', '/p:VcpkgEnableManifest=false', "/p:ApexVcpkgRoot=$dependencies\", "/p:OutDir=$output\", "/p:IntDir=$intermediate\")
if ($Flavor -eq 'Public') { $argsList += '/p:ApexFlavorDefines=APEX_NO_DEV_TOOLS' }
else { $argsList += '/p:ApexFlavorDefines=' }
New-Item -ItemType Directory -Path (Join-Path $repo 'outputs/build') -Force | Out-Null
$recipe = @{ executable=$msbuild; args=$argsList; target="Release/x86/$Flavor"; artifacts=@("$relativeOutput/ApexRadiance.asi"); requires=@($dependencies) }
$recipePath = Join-Path $repo "outputs/build/$Flavor-recipe.json"
$recipe | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recipePath -Encoding utf8
Push-Location $repo
try {
    & node (Join-Path $repo '.agents/skills/apex-compile-project/scripts/build.mjs') run --repo $repo --recipe $recipePath
    if ($LASTEXITCODE -ne 0) { throw "$Flavor build failed." }
    $artifact = Join-Path $output 'ApexRadiance.asi'
    Get-FileHash -LiteralPath $artifact -Algorithm SHA256
} finally { Pop-Location }


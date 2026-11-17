param([Parameter(Mandatory=$true)][string]$Precomp, [string]$D3d9 = '', [string]$OutDir = (Join-Path $env:TEMP 'ApexGBufferCheck'))
# G-buffer shader copies, offline check (read-only: prints to stdout; the compiler's outputs stay in $OutDir)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$exe = Join-Path $OutDir 'gbuffer_check.exe'
Push-Location $OutDir
try {
    & cl.exe /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MT ('/Fe' + $exe) (Join-Path $PSScriptRoot 'check.cpp') (Join-Path $repo 'features/shader_patches.cpp') d3dcompiler.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'G-buffer check did not compile' }
    $families = Join-Path $OutDir 'families.txt' # technique -> its pixel shaders, for the counts per family
    $rules = Join-Path $OutDir 'rule-classes.txt' # albedo rule shaders -> sampler classes, for the rule check on the GPU
    & node (Join-Path $repo 'tools/gbuffer_ids/generate.cjs') (Resolve-Path -LiteralPath $Precomp).Path '' $families $rules | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'families file not made' }
    # Ambient Occlusion's HLSL, for its G-buffer consumers on the GPU (GB-04)
    $ao = [regex]::Match((Get-Content -LiteralPath (Join-Path $repo 'patches/ambient_occlusion_patch.cpp') -Raw), 'R"HLSL\((.*?)\)HLSL"', [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $ao.Success) { throw 'HLSL block missing in ambient_occlusion_patch.cpp' }
    $aoHlsl = Join-Path $OutDir 'ambient_occlusion.hlsl'
    Set-Content -LiteralPath $aoHlsl -Value $ao.Groups[1].Value -Encoding ascii
    $extra = @('--families', $families, '--rules', $rules, '--ao', $aoHlsl) + $(if ($D3d9) { @('--d3d9', (Resolve-Path -LiteralPath $D3d9).Path) } else { @() })
    & $exe (Resolve-Path -LiteralPath $Precomp).Path @extra
    if ($LASTEXITCODE -ne 0) { throw 'G-buffer check failed' }
} finally { Pop-Location }

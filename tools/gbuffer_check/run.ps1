param([Parameter(Mandatory=$true)][string]$Precomp, [string]$D3d9 = '', [string[]]$Only = @(), [string]$OutDir = (Join-Path $env:TEMP 'ApexGBufferCheck'))
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
    # The HLSL the checks compile, from the production sources: Ambient Occlusion's (GB-04), the G-buffer passes and the
    # temporal AA (gbuffer_patch.cpp, in that order), the movers' pixel shader (GB-05)
    . (Join-Path $PSScriptRoot 'hlsl_blocks.ps1')
    $ao = Get-HlslBlocks (Join-Path $repo 'patches/ambient_occlusion_patch.cpp')
    $gb = Get-HlslBlocks (Join-Path $repo 'patches/gbuffer_patch.cpp')
    $mv = Get-HlslBlocks (Join-Path $repo 'features/gbuffer_movers.cpp')
    if ($ao.Count -lt 1 -or $gb.Count -lt 2 -or $mv.Count -lt 1) { throw 'HLSL blocks missing (ambient_occlusion_patch.cpp, gbuffer_patch.cpp, gbuffer_movers.cpp)' }
    $hlsl = @{ ao = $ao[0]; gb = $gb[0]; taa = $gb[1]; mover = $mv[0] }
    $hlslArgs = @()
    foreach ($k in 'ao', 'gb', 'taa', 'mover') {
        $file = Join-Path $OutDir "$k.hlsl"
        Set-Content -LiteralPath $file -Value $hlsl[$k] -Encoding ascii
        $hlslArgs += @("--$k", $file)
    }
    $extra = @('--families', $families, '--rules', $rules) + $hlslArgs + $(if ($Only) { @('--only', ($Only -join ',')) } else { @() }) + $(if ($D3d9) { @('--d3d9', (Resolve-Path -LiteralPath $D3d9).Path) } else { @() })
    & $exe (Resolve-Path -LiteralPath $Precomp).Path @extra
    if ($LASTEXITCODE -ne 0) { throw 'G-buffer check failed' }
} finally { Pop-Location }

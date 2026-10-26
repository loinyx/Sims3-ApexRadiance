param([string]$OutDir=(Join-Path $env:TEMP 'ApexRenderCostChecks'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
$source=Get-Content -LiteralPath (Join-Path $repo 'features/refpack_codec.cpp') -Raw
$blocks=@([regex]::Match($source,'(?m)^constexpr uint32_t kMaxLen[^\r\n]*').Value)
foreach($name in @('inline bool CanImproveMatch','inline int Cost')) {
    $m=[regex]::Match($source, '(?ms)^'+[regex]::Escape($name)+'\([^\n]*\) \{.*?^\}')
    if(-not $m.Success) { throw ('Candidate block missing: '+$name) }
    $blocks += $m.Value
}
[IO.File]::WriteAllText((Join-Path $OutDir 'refpack_candidate.h'),($blocks -join "`n"),[Text.UTF8Encoding]::new($false))
$hooks=Get-Content -LiteralPath (Join-Path $repo 'framework/d3d9_hooks.cpp') -Raw
$hook=[regex]::Match($hooks,'(?ms)^HRESULT STDMETHODCALLTYPE H_CreateTexture\([^\n]*\) \{.*?^\}')
if(-not $hook.Success) { throw 'Texture allocation hook missing' }
[IO.File]::WriteAllText((Join-Path $OutDir 'texture_allocation_hook.h'),$hook.Value,[Text.UTF8Encoding]::new($false))
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /Oi /fp:precise /MT ('/I'+$OutDir) ('/I'+(Join-Path $repo 'features')) /Ferender_cost_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Render cost checks compilation failed' }
    & '.\render_cost_checks.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Render cost checks failed' }
} finally { Pop-Location }

param([string]$Repo = (Join-Path $PSScriptRoot '../..'), [string]$OutDir = (Join-Path $env:TEMP 'ApexLightingResourceTests'))
$ErrorActionPreference='Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir=(Resolve-Path -LiteralPath $OutDir).Path
$sourcePath=Join-Path $Repo 'features/lot_light_bridge.cpp'
$source=Get-Content -LiteralPath $sourcePath -Raw
function Extract-Block([string]$pattern) {
    $m=[regex]::Match($source,$pattern)
    if(-not $m.Success) { throw ('Production block missing: '+$pattern) }
    return $m.Value
}
$sampler=Extract-Block '(?s)struct SamplerBind \{.*?\r?\n\};'
$gain=Extract-Block '(?s)struct ConstGain \{.*?\r?\n\};'
$constX=Extract-Block '(?s)struct ConstX \{.*?\r?\n\};'
$weighted=Extract-Block '(?m)^float NightWeighted\(.*\r?\nfloat GroundGain\(.*\r?\nfloat RoadGain\(.*\r?\nfloat LotMapGain\(.*'
$nativeScale=Extract-Block '(?m)^bool NativeWallFresh\(.*\r?\nfloat NativeWallScale\(.*'
$wall=Extract-Block '(?s)template <typename DrawFn> bool DrawWallGain\(.*?\r?\n\}'
$cone=Extract-Block '(?s)float LampConeAt\(.*?\r?\n\}'
$hasCone=Extract-Block '(?m)^bool HasCone\(.*'
$picked="float LampConeVisibility(const LampCone&, const float*, const float*, float);`n"+(Extract-Block '(?s)float PickedLampVisibility\(.*?\r?\n\}')
# Keep both implementations and their production memo wrappers.
$pickerStart=$source.IndexOf('int PickObjectLampsImpl(')
$pickerEnd=$source.IndexOf('// The colour an outdoor object', $pickerStart)
if($pickerStart -lt 0 -or $pickerEnd -lt 0){throw 'Object picker boundaries missing'}
$picker=$source.Substring($pickerStart,$pickerEnd-$pickerStart)
$fenceStart=$source.IndexOf('int PickFenceLampsImpl(')
$fenceEnd=$source.IndexOf('template <typename DrawFn> bool DrawInstanced(', $fenceStart)
if($fenceStart -lt 0 -or $fenceEnd -lt 0){throw 'Fence picker boundaries missing'}
$fencePicker=$source.Substring($fenceStart,$fenceEnd-$fenceStart)
$object=Extract-Block '(?s)template <typename DrawFn> bool DrawObjectLamp\(.*?\r?\n\}'
$instanced=Extract-Block '(?s)template <typename DrawFn> bool DrawInstanced\(.*?\r?\n\}'
$core=$source.IndexOf('template <typename DrawFn> D3D9Hooks::HookAction OnDrawInnerCore(')
if($core -lt 0) { throw 'OnDrawInnerCore not found' }
$lotStart=$source.IndexOf('    EnsureReplacement(dev);',$core)
$lotEnd=$source.IndexOf('// Foliage vertex shaders are swapped',$lotStart)
if($lotStart -lt 0 -or $lotEnd -lt 0) { throw 'Lot branch boundaries missing' }
$lot=$source.Substring($lotStart,$lotEnd-$lotStart).TrimEnd()
if(-not $lot.StartsWith('    EnsureReplacement(dev);') -or -not $lot.EndsWith('}')) { throw 'Unexpected lot branch bounds' }
$prefix="template <typename DrawFn> D3D9Hooks::HookAction ExtractedLotBranch(IDirect3DDevice9* dev, DrawFn draw) {`n    constexpr auto kContinue = D3D9Hooks::HookAction::Continue;`n"
$snowEdge=Extract-Block '(?s)struct SnowEdgeBind \{.*?\r?\n\};'
$arrival=Extract-Block '(?s)void NoteLotDraw\(.*?\r?\n\}'
$arrivalFixture=@"
namespace ArrivalFixture {
std::atomic<DWORD> g_lotDrawTick{0};
std::unordered_map<uint64_t,DWORD> g_lotDrawSeen;
std::unordered_set<uint64_t> g_lotArrivals;
namespace LotLightBridge { int requests=0; void RequestLampEditRefresh(){++requests;} }
$arrival
}
"@
$extracted=$sampler+"`n"+$gain+"`n"+$constX+"`n"+$weighted+"`n"+$nativeScale+"`n"+$wall+"`n"+$cone+"`n"+$hasCone+"`n"+$picked+"`n"+$picker+"`n"+$fencePicker+"`n"+$object+"`n"+$instanced+"`n"+$snowEdge+"`n"+$prefix+$lot+"`n"
$extracted += $arrivalFixture
$header=Join-Path $OutDir 'extracted_resource_paths.h'
Set-Content -LiteralPath $header -Value $extracted -Encoding UTF8
$sourceHash=(Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
$extractedHash=(Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash
Write-Output ('Production lot_light_bridge SHA256: '+$sourceHash)
Write-Output ('Extracted resource paths SHA256: '+$extractedHash)
. (Join-Path $PSScriptRoot 'compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W4 /Od /RTC1 /MT ('/I'+$Repo) ('/I'+$OutDir) /Feresource_paths_audit.exe (Join-Path $PSScriptRoot 'resource_paths_fixture.cpp')
    if($LASTEXITCODE -ne 0) { throw 'Resource path fixture compilation failed' }
    & './resource_paths_audit.exe'
    if($LASTEXITCODE -ne 0) { throw 'Resource path checks failed' }
} finally { Pop-Location }

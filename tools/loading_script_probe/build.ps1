param(
    [Parameter(Mandatory=$true)][string]$References,
    [Parameter(Mandatory=$true)][string]$Python,
    [string]$ScriptResourceLibrary=(Join-Path $env:ProgramFiles 's3pe/s3pi.ScriptResource.dll'),
    [string]$OutDir=(Join-Path $env:TEMP 'ApexLoadingScriptStudy')
)
$ErrorActionPreference='Stop'
$OutDir=[IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$compiler=Join-Path $env:WINDIR 'Microsoft.NET/Framework/v3.5/csc.exe'
$dll=Join-Path $OutDir 'ApexLoadingStudy.dll'
$arguments=@('/nologo','/target:library','/optimize+','/nostdlib+','/noconfig',('/out:'+$dll))
foreach($name in @('mscorlib','System','System.Xml','SimIFace','ScriptCore','Sims3Metadata','Sims3GameplaySystems','UI')) {
    $reference=Join-Path $References ($name+'.dll')
    if(-not (Test-Path -LiteralPath $reference)){throw ('Missing game reference: '+$name)}
    $arguments+=('/reference:'+$reference)
}
$arguments+=(Join-Path $PSScriptRoot 'Probe.cs')
& $compiler @arguments
if($LASTEXITCODE -ne 0){throw 'Script observer compilation failed'}
Push-Location -LiteralPath $OutDir
try {
    & $compiler /nologo /out:ProbeChecks.exe (Join-Path $PSScriptRoot 'Probe.cs') (Join-Path $PSScriptRoot 'Check.cs')
    if($LASTEXITCODE -ne 0){throw 'Script observer checks compilation failed'}
    & './ProbeChecks.exe'
    if($LASTEXITCODE -ne 0){throw 'Script observer checks failed'}
} finally {Pop-Location}
[Reflection.Assembly]::LoadFrom((Join-Path (Split-Path $ScriptResourceLibrary) 's3pi.Interfaces.dll')) | Out-Null
[Reflection.Assembly]::LoadFrom($ScriptResourceLibrary) | Out-Null
$resource=[ScriptResource.ScriptResource]::new(1,$null)
$resource.Version=2
$resource.GameVersion='0.0.0.32'
$resource.Unknown2=734328735
$bytes=[IO.File]::ReadAllBytes($dll)
$resource.Assembly=[IO.BinaryReader]::new([IO.MemoryStream]::new($bytes))
$s3sa=Join-Path $OutDir 'ApexLoadingStudy.s3sa'
[IO.File]::WriteAllBytes($s3sa,$resource.AsBytes)
$roundtrip=[ScriptResource.ScriptResource]::new(1,[IO.MemoryStream]::new($resource.AsBytes))
$decoded=$roundtrip.Assembly.ReadBytes([int]$roundtrip.Assembly.BaseStream.Length)
if([Convert]::ToBase64String($decoded) -ne [Convert]::ToBase64String($bytes)){throw 'Script resource roundtrip failed'}
& $Python -I (Join-Path $PSScriptRoot 'package.py') $s3sa (Join-Path $PSScriptRoot 'Bootstrap.xml') (Join-Path $OutDir 'ApexRadiance_LoadingScriptStudy_test2.package')
if($LASTEXITCODE -ne 0){throw 'Script observer package validation failed'}

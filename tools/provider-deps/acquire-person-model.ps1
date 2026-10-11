[CmdletBinding()]
param([string]$ModelDirectory='', [string]$StageDirectory='', [string]$RuntimeSdk='', [switch]$Offline)
$ErrorActionPreference='Stop'
# Resolve defaults after parameter binding. PowerShell 5.1 invocation and empty
# forwarded arguments must both resolve against this script, never the caller CWD.
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if([string]::IsNullOrWhiteSpace($ModelDirectory)){$ModelDirectory=Join-Path $repo 'build\models\pphumanseg'}
if([string]::IsNullOrWhiteSpace($RuntimeSdk)){$RuntimeSdk=Join-Path $repo 'build\provider-deps\ort-sdk'}
$revision='47534e27c9851bb1128ccc0102f1145e27f23f98'
$hash='552D8A984054E59B5D773D24B9B12022B22046CEB2BBC4C9AAEACEB36A9DDF24'
$runtimeHash='903C92C54ACC57CAA77D44D4C856C829EB82F5CF6755B47367A8153BD3FABEB6'
$url="https://media.githubusercontent.com/media/opencv/opencv_zoo/$revision/models/human_segmentation_pphumanseg/human_segmentation_pphumanseg_2023mar.onnx"
function AssertHash([string]$path,[string]$expected){
    if(!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash-ne$expected){throw "Pinned dependency unavailable or changed: $path"}
}
$directory=[IO.Path]::GetFullPath($ModelDirectory)
$model=Join-Path $directory 'model.onnx'
if(!(Test-Path -LiteralPath $model -PathType Leaf) -or (Get-FileHash -LiteralPath $model).Hash-ne$hash){
    if($Offline){throw "Pinned person model unavailable in offline mode: $model"}
    New-Item -ItemType Directory -Force -Path $directory|Out-Null
    $temporary=Join-Path $directory ('model-download-'+[guid]::NewGuid().ToString('N')+'.tmp')
    try{
        Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing
        if((Get-Item -LiteralPath $temporary).Length-ne6163938 -or (Get-FileHash -LiteralPath $temporary).Hash-ne$hash){throw 'Official pinned model integrity check failed'}
        Move-Item -LiteralPath $temporary -Destination $model -Force
    }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
}
if((Get-Item -LiteralPath $model).Length-ne6163938){throw 'Pinned person model size mismatch'}
$assets=@(
    @{source=(Join-Path $PSScriptRoot 'pphumanseg-manifest.json');name='manifest.json';hash='5FB6D31CC4D87DBF90CA1B9555720E486B0A262DD07D37FB48C93541D4D6D48B'},
    @{source=(Join-Path $PSScriptRoot 'PP-HumanSeg-LICENSE.txt');name='LICENSE';hash='D9CF3B547FA4232FB9285F2DB52E909F10F00781AAF64A6D0BB90C55149F1363'},
    @{source=(Join-Path $PSScriptRoot 'PP-HumanSeg-NOTICE.txt');name='NOTICE';hash='5D0AABDF475A5625EBA5B9E984DFF1CD771EC871A816F4214ACEDB3AABAF560A'}
)
foreach($asset in $assets){AssertHash $asset.source $asset.hash}
$runtimeAssets=@()
if($StageDirectory){
    $sdk=[IO.Path]::GetFullPath($RuntimeSdk)
    $runtimeAssets=@(
        @{source=(Join-Path $sdk 'bin\onnxruntime.dll');relative='onnxruntime.dll';hash=$runtimeHash},
        @{source=(Join-Path $sdk 'LICENSE');relative='licenses\onnxruntime\LICENSE';hash='C250D6278F0B47A6439FB7592B08B58A55EB9F535AA49A1DB63211C3F982B674'},
        @{source=(Join-Path $sdk 'ThirdPartyNotices.txt');relative='licenses\onnxruntime\ThirdPartyNotices.txt';hash='FAC5BB85F568B38FD8F3A6AEA32737985841F23A1A43E9CE952F79474F883AA9'}
    )
    foreach($asset in $runtimeAssets){AssertHash $asset.source $asset.hash}
}
foreach($asset in $assets){Copy-Item -LiteralPath $asset.source -Destination (Join-Path $directory $asset.name) -Force}
$receipt=[ordered]@{model='PP-HumanSeg2023mar';revision=$revision;url=$url;sha256=$hash.ToLowerInvariant();sizeBytes=6163938;license='Apache-2.0';files=@();stage=$null;stagedFiles=@();actualExitCode=0}
foreach($name in @('model.onnx','manifest.json','LICENSE','NOTICE')){
    $file=Join-Path $directory $name
    $receipt.files+=@{path=$file;sha256=(Get-FileHash -LiteralPath $file).Hash;sizeBytes=(Get-Item -LiteralPath $file).Length}
}
if($StageDirectory){
    $stage=[IO.Path]::GetFullPath($StageDirectory)
    $stageAssets=@(@{source=$model;relative='models\pphumanseg\model.onnx';hash=$hash})
    foreach($asset in $assets){$stageAssets+=@{source=(Join-Path $directory $asset.name);relative=('models\pphumanseg\'+$asset.name);hash=$asset.hash}}
    $stageAssets+=$runtimeAssets
    foreach($asset in $stageAssets){
        $target=Join-Path $stage $asset.relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)|Out-Null
        Copy-Item -LiteralPath $asset.source -Destination $target -Force
        AssertHash $target $asset.hash
        $receipt.stagedFiles+=@{path=$target;relativePath=$asset.relative;sha256=(Get-FileHash -LiteralPath $target).Hash;sizeBytes=(Get-Item -LiteralPath $target).Length}
    }
    $receipt.stage=$stage;$receipt.runtimeSha256=$runtimeHash
    $receipt|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $stage 'segmentation-dependencies.receipt.json') -Encoding utf8
}
$receipt|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $directory 'acquisition.receipt.json') -Encoding utf8
Write-Host "Pinned PP-HumanSeg model ready: $directory"
if($StageDirectory){Write-Host "Verified runtime, model and notices staged: $stage"}

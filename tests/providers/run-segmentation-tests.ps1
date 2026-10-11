[CmdletBinding()]
param([string]$ModelDirectory='',[string]$RuntimeSdk='',[ValidateRange(1,600)][int]$TimeoutSeconds=120)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
if(!$ModelDirectory){$ModelDirectory=Join-Path $repo 'build\models\pphumanseg'}
if(!$RuntimeSdk){$RuntimeSdk=Join-Path $repo 'build\provider-deps\ort-sdk'}
$run=Join-Path $repo ('build\segmentation-verification\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $run|Out-Null
$receipt=[ordered]@{startedUtc=(Get-Date).ToUniversalTime().ToString('o');processes=@();hashes=@();sourceHashes=@();sourcesUnchanged=$false;completed=$false;actualExitCode=1;failure=$null}
function Quote([string]$value){if($value.Contains('"') -or $value.EndsWith('\')){throw 'Invalid child argument'};return '"'+$value+'"'}
function Recorded([string]$exe,[string[]]$arguments,[string]$label){
    $psi=New-Object Diagnostics.ProcessStartInfo;$psi.FileName=$exe;$psi.Arguments=($arguments|ForEach-Object{Quote $_})-join' '
    $psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.WorkingDirectory=$run;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $psi.EnvironmentVariables['PATH']='C:\vcpkg\installed\x64-windows\bin;'+$env:PATH
    $p=New-Object Diagnostics.Process;$p.StartInfo=$psi;$watch=[Diagnostics.Stopwatch]::StartNew();$timedOut=$false
    try{if(!$p.Start()){throw 'Child launch failed'};$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        if(!$p.WaitForExit($TimeoutSeconds*1000)){$timedOut=$true;$p.Kill();$p.WaitForExit();$exit=124}else{$p.WaitForExit();$exit=$p.ExitCode}
        $stdout=$out.GetAwaiter().GetResult();$stderr=$err.GetAwaiter().GetResult();[IO.File]::WriteAllText((Join-Path $run "$label.stdout.log"),$stdout);[IO.File]::WriteAllText((Join-Path $run "$label.stderr.log"),$stderr)
        Write-Host $stdout;Write-Host $stderr
        $receipt.processes+=@{label=$label;command=$exe;arguments=$arguments;actualExitCode=$exit;timedOut=$timedOut;elapsedMs=$watch.Elapsed.TotalMilliseconds}
        return $exit
    }finally{$watch.Stop();$p.Dispose()}
}
try{
    if((Get-FileHash (Join-Path $ModelDirectory 'model.onnx')).Hash-ne'552D8A984054E59B5D773D24B9B12022B22046CEB2BBC4C9AAEACEB36A9DDF24'){throw 'Pinned model hash mismatch'}
    if((Get-FileHash (Join-Path $RuntimeSdk 'bin\onnxruntime.dll')).Hash-ne'903C92C54ACC57CAA77D44D4C856C829EB82F5CF6755B47367A8153BD3FABEB6'){throw 'Pinned runtime hash mismatch'}
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    $vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $sources=@('providers\onnx_runner.cpp','providers\frame_preprocessor.cpp','providers\result_adapter.cpp','providers\analysis_worker.cpp','providers\segmentation_service.cpp','effects\analysisbridge\analysis_mailbox.cpp')|ForEach-Object{Join-Path $repo "windows\src\$_"}
    $sources+=Join-Path $PSScriptRoot 'segmentation_service_tests.cpp'
    $inputs=@(& rg --files (Join-Path $repo 'windows\src\providers'))
    $inputs+=@(& rg --files (Join-Path $repo 'windows\src\effects\analysisbridge'))
    $inputs+=@($PSCommandPath,(Join-Path $PSScriptRoot 'segmentation_service_tests.cpp'))
    foreach($inputPath in $inputs|Sort-Object -Unique){$receipt.sourceHashes+=@{path=$inputPath;sha256=(Get-FileHash -LiteralPath $inputPath).Hash}}
    $exe=Join-Path $run 'segmentation-tests.exe';$obj=Join-Path $run 'obj';New-Item -ItemType Directory -Path $obj|Out-Null
    $compiler=@('/nologo','/O2','/EHsc','/std:c++17','/W4','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',('/I'+(Join-Path $RuntimeSdk 'include')),'/IC:\vcpkg\installed\x64-windows\include',('/I'+(Join-Path $repo 'windows\src')))+$sources
    $command='call '+(Quote $vcvars)+' >nul 2>&1'+[Environment]::NewLine+'cl.exe '+(($compiler|ForEach-Object{Quote $_})-join' ')+' /Fo"'+$obj+'\\" /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" swscale.lib avutil.lib ole32.lib /OUT:"'+$exe+'"'+[Environment]::NewLine+'exit /b %errorlevel%'
    $batch=Join-Path $run 'compile.cmd';[IO.File]::WriteAllText($batch,$command)
    if((Recorded $env:ComSpec @('/d','/c',$batch) 'compile')-ne0){throw 'Compile failed'}
    if((Recorded $exe @($ModelDirectory,(Join-Path $RuntimeSdk 'bin\onnxruntime.dll')) 'real-model-tests')-ne0){throw 'Real-model tests failed'}
    foreach($source in $receipt.sourceHashes){if((Get-FileHash -LiteralPath $source.path).Hash-ne$source.sha256){throw "Source changed during verification: $($source.path)"}}
    $receipt.sourcesUnchanged=$true;$receipt.completed=$true;$receipt.actualExitCode=0
}catch{$receipt.failure=$_.Exception.Message;Write-Host "FAILED: $($receipt.failure)"}
finally{
    $paths=@(& rg --files (Join-Path $repo 'windows\src\providers'))+@($PSCommandPath,(Join-Path $PSScriptRoot 'segmentation_service_tests.cpp'))
    $paths+=@(Get-ChildItem -LiteralPath $ModelDirectory -File|ForEach-Object{$_.FullName})
    $paths+=@(Get-ChildItem -LiteralPath $run -Recurse -File|ForEach-Object{$_.FullName})
    $paths+=@((Join-Path $RuntimeSdk 'bin\onnxruntime.dll'))
    foreach($path in $paths|Sort-Object -Unique){if(Test-Path -LiteralPath $path){$receipt.hashes+=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}}}
    $receipt.finishedUtc=(Get-Date).ToUniversalTime().ToString('o');$receipt|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $run 'receipt.json') -Encoding utf8
    Write-Host "RECEIPT: $(Join-Path $run 'receipt.json')"
}
exit $receipt.actualExitCode

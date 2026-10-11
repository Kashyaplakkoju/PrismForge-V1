[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$CompiledDirectory,[ValidateRange(5,240)][int]$TimeoutSeconds=180)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$compiled=(Resolve-Path -LiteralPath $CompiledDirectory).Path
$run=Join-Path $repo ('build\pack-regressions-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$receipt=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;actualExitCode=1;reusedCompileDirectory=$compiled;sourceHashes=@();objectHashes=@();processes=@();error=$null}
$receiptPath=Join-Path $run 'receipt.json'
function Save {$receipt|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $receiptPath -Encoding UTF8}
function Native([string]$Name,[string]$Command) {
 $bat=Join-Path $run ($Name+'.cmd');$out=Join-Path $run ($Name+'.stdout.log');$err=Join-Path $run ($Name+'.stderr.log')
 [IO.File]::WriteAllText($bat,'@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+$Command+"`r`n"+'exit /b %ERRORLEVEL%'+"`r`n",[Text.UTF8Encoding]::new($false))
 $watch=[Diagnostics.Stopwatch]::StartNew();$process=$null
 try{
  $process=Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('"'+$bat+'"')) -WorkingDirectory $repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
  $null=$process.Handle;$timedOut=!$process.WaitForExit($TimeoutSeconds*1000)
  if($timedOut){& taskkill.exe /PID $process.Id /T /F | Out-Null;$process.WaitForExit();$exit=124}else{$process.WaitForExit();$exit=$process.ExitCode}
  $receipt.processes+=@{name=$Name;actualExitCode=$exit;timedOut=$timedOut;elapsedMs=$watch.Elapsed.TotalMilliseconds;command=$Command;stdout=$out;stderr=$err;stdoutSha256=(Get-FileHash -LiteralPath $out).Hash;stderrSha256=(Get-FileHash -LiteralPath $err).Hash};Save
  Get-Content -LiteralPath $out -Tail 6
  if((Get-Item -LiteralPath $err).Length){Get-Content -LiteralPath $err -Tail 8}
  if($exit-ne0){throw "$Name failed actual exit=$exit"}
 }finally{if($process){$process.Dispose()};$watch.Stop()}
}
try{
 $compileReceipt=Get-Content -LiteralPath (Join-Path $compiled 'production-compile.receipt.json') -Raw|ConvertFrom-Json
 if($compileReceipt.actualExitCode-ne0){throw 'Reused production compile was not successful'}
 $inputs=Get-Content -LiteralPath (Join-Path $compiled 'production-source-hashes.json') -Raw|ConvertFrom-Json
 foreach($compiledInput in $inputs){$path=if([IO.Path]::IsPathRooted($compiledInput.path)){$compiledInput.path}else{Join-Path $repo $compiledInput.path};if((Get-FileHash -LiteralPath $path).Hash-ne$compiledInput.sha256){throw "Compiled input drifted: $path"};$receipt.sourceHashes+=@{path=$path;sha256=$compiledInput.sha256}}
 $objects=@(Get-ChildItem -LiteralPath $compiled -Filter '*.obj'|Where-Object{$_.BaseName-ne'pack_product_tests'})
 foreach($o in $objects){$receipt.objectHashes+=@{path=$o.FullName;sha256=(Get-FileHash -LiteralPath $o.FullName).Hash}}
 $fixtures=@(
 @{name='desktop_looks';path=(Join-Path $repo 'tests\integration-engine\desktop_looks_tests.cpp');args=''},
 @{name='combined_fallback';path=(Join-Path $repo 'tests\integration-engine\combined_fallback_tests.cpp');args=''},
 @{name='conformance';path=(Join-Path $repo 'tests\integration-engine\conformance_tests.cpp');args=' --conformance --guards --faults --switches --fanout --soak --regressions'},
 @{name='desktop_studio';path=(Join-Path $repo 'tests\desktopstudio\desktop_studio_tests.cpp');args=''},
 @{name='custom_lighting';path=(Join-Path $repo 'tests\integration-engine\custom_lighting_tests.cpp');args=''})
 foreach($f in $fixtures){$receipt.sourceHashes+=@{path=$f.path;sha256=(Get-FileHash -LiteralPath $f.path).Hash}}
 $receipt.sourceHashes+=@{path=$PSCommandPath;sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash}
 $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe";$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim();$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
 $ff='C:\vcpkg\installed\x64-windows';$sdk=Join-Path $repo 'build\provider-deps\ort-sdk'
 $assets=Join-Path $repo 'build\pack-final-distribution-20261009-r1'
 foreach($dll in Get-ChildItem -LiteralPath $assets -Filter '*.dll'){Copy-Item -LiteralPath $dll.FullName -Destination $run}
 Copy-Item -LiteralPath (Join-Path $assets 'models') -Destination $run -Recurse
 New-Item -ItemType Directory -Path (Join-Path $run 'res\accessories') -Force | Out-Null
 foreach($file in Get-ChildItem -LiteralPath (Join-Path $assets 'res\accessories') -File){Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $run 'res\accessories')}
 $objectArgs=($objects|ForEach-Object{'"'+$_.FullName+'"'})-join' '
 New-Item -ItemType Directory -Path (Join-Path $run 'temp') | Out-Null
 foreach($fixture in $fixtures){
  $exe=Join-Path $run ($fixture.name+'.exe')
  $command='cl.exe /nologo /O2 /openmp /EHsc /std:c++17 /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 /I"'+$ff+'\include" /I"'+$repo+'\windows\src" /I"'+$repo+'" /I"'+$sdk+'\include" "'+$fixture.path+'" /Fo"'+$run+'\'+$fixture.name+'.obj" '+$objectArgs+' /link /LIBPATH:"'+$ff+'\lib" ole32.lib psapi.lib d3d11.lib d3dcompiler.lib dxgi.lib avutil.lib swscale.lib avcodec.lib avformat.lib windowscodecs.lib bcrypt.lib /OUT:"'+$exe+'"'
  Native ($fixture.name+'-compile') $command
  Native ($fixture.name+'-run') ('set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'set "TEMP='+$run+'\temp"'+"`r`n"+'set "TMP='+$run+'\temp"'+"`r`n"+'"'+$exe+'"'+$fixture.args)
 }
 $receipt.actualExitCode=0
}catch{$receipt.error=$_.Exception.Message;$receipt.errorLocation=$_.ScriptStackTrace;Write-Output ('FAILED '+$receipt.error)}finally{
 $receipt.sourcesUnchanged=$true;foreach($item in $receipt.sourceHashes){if((Get-FileHash -LiteralPath $item.path).Hash-ne$item.sha256){$receipt.sourcesUnchanged=$false;$receipt.actualExitCode=1}}
 $receipt.completed=$true;$receipt.completedUtc=[DateTime]::UtcNow.ToString('o');Save;Write-Output ('Receipt: '+$receiptPath)
}
exit $receipt.actualExitCode
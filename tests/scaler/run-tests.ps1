[CmdletBinding()]
param([ValidateRange(1,600)][int]$TimeoutSeconds=90)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$run=Join-Path $repo ('build\scaler-tests\'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $run | Out-Null
$receipt=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;actualExitCode=1;sources=@();processes=@();evidence=@();scope='Exact-format scaler correctness and paired1080p fixture-only processing; excludes capture/effects/publication/receivers'}
$ff='C:\vcpkg\installed\x64-windows'
function Child([string]$label,[string]$command){
 $script=Join-Path $run ($label+'.cmd');[IO.File]::WriteAllText($script,'@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+$command+"`r`n"+'exit /b %ERRORLEVEL%'+"`r`n",[Text.ASCIIEncoding]::new())
 $stdout=Join-Path $run ($label+'.stdout.log');$stderr=Join-Path $run ($label+'.stderr.log');$watch=[Diagnostics.Stopwatch]::StartNew();$p=$null
 try{$p=Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('"'+$script+'"')) -WorkingDirectory $repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr;$null=$p.Handle;$timedOut=!$p.WaitForExit($TimeoutSeconds*1000);if($timedOut){& taskkill.exe /PID $p.Id /T /F|Out-Null;$exit=124}else{$p.WaitForExit();$exit=$p.ExitCode};$receipt.processes+=@{label=$label;actualExitCode=$exit;timedOut=$timedOut;elapsedMs=$watch.Elapsed.TotalMilliseconds};Get-Content -LiteralPath $stdout -Tail 8;if((Get-Item $stderr).Length){Get-Content -LiteralPath $stderr -Tail 8};if($exit-ne0){throw "$label exit=$exit"}}finally{$watch.Stop();if($p){$p.Dispose()}}
}
try{
 $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe";$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim();$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
 $paths=@('windows/src/video/framescaler.cpp','windows/src/video/framescaler.h','windows/src/video/directshowscaler.cpp','windows/src/video/directshowscaler.h','windows/src/video/guipreviewscaler.cpp','windows/src/video/guipreviewscaler.h','windows/src/video/1_1scaler.cpp','windows/src/video/1_1scaler.h','tests/scaler/scaler_tests.cpp','tests/scaler/run-tests.ps1')
 foreach($path in $paths){$receipt.sources+=@{path=$path;sha256=(Get-FileHash -LiteralPath (Join-Path $repo $path)).Hash}}
 $baseline=Join-Path $run 'baseline';New-Item -ItemType Directory -Path (Join-Path $baseline 'video')|Out-Null
 $receipt.baselineCommit=(& git -C $repo rev-parse HEAD).Trim()
 foreach($name in @('framescaler.cpp','framescaler.h','directshowscaler.cpp','directshowscaler.h','guipreviewscaler.cpp','guipreviewscaler.h','1_1scaler.cpp','1_1scaler.h')){$text=& git -C $repo show ($receipt.baselineCommit+':windows/src/video/'+$name);if($LASTEXITCODE-ne0){throw 'Cannot read baseline'};[IO.File]::WriteAllLines((Join-Path $baseline ('video\'+$name)),[string[]]$text,[Text.UTF8Encoding]::new($false))}
 foreach($variant in @('current','baseline')){
  $src=if($variant-eq'current'){Join-Path $repo 'windows\src'}else{$baseline};$obj=Join-Path $run ($variant+'-obj');New-Item -ItemType Directory -Path $obj|Out-Null;$exe=Join-Path $run ($variant+'.exe')
  $command='cl.exe /nologo /O2 /EHsc /std:c++17 /W3 /DNOMINMAX /I"'+$src+'" /I"'+$repo+'\windows\src" /I"'+$ff+'\include" "'+$PSScriptRoot+'\scaler_tests.cpp" "'+$src+'\video\framescaler.cpp" "'+$src+'\video\directshowscaler.cpp" "'+$src+'\video\guipreviewscaler.cpp" "'+$src+'\video\1_1scaler.cpp" /Fo"'+$obj.Replace('\','/')+'/" /link /LIBPATH:"'+$ff+'\lib" avutil.lib avcodec.lib avformat.lib swscale.lib /OUT:"'+$exe+'"'
  Child ($variant+'-compile') $command
 }
 Child 'current-tests' ('set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'"'+$run+'\current.exe"')
 $index=0;foreach($variant in @('baseline','current','current','baseline')){++$index;Child ('bench'+$index+'-'+$variant) ('set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'"'+$run+'\'+$variant+'.exe" --benchmark')}
 foreach($row in $receipt.sources){if((Get-FileHash -LiteralPath (Join-Path $repo $row.path)).Hash-ne$row.sha256){throw 'Source changed during verification'}}
 $receipt.completed=$true;$receipt.actualExitCode=0
}catch{$receipt.failure=$_.Exception.Message;Write-Host ('FAILED '+$receipt.failure)}finally{
 foreach($path in @(Get-ChildItem -LiteralPath $run -Recurse -File|ForEach-Object{$_.FullName})){ $hash=$null;for($retry=0;$retry-lt10;$retry++){try{$hash=(Get-FileHash -LiteralPath $path -ErrorAction Stop).Hash;break}catch{Start-Sleep -Milliseconds 100}};if($hash){$receipt.evidence+=@{path=$path;sha256=$hash}}else{$receipt.actualExitCode=1;$receipt.failure="Cannot bind evidence hash: $path"}};$receipt.finishedUtc=[DateTime]::UtcNow.ToString('o');$receipt|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $run 'receipt.json') -Encoding utf8;Write-Output ('SCALER_RECEIPT '+(Join-Path $run 'receipt.json'))
}
exit $receipt.actualExitCode

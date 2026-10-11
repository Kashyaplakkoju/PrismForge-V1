[CmdletBinding()]
param([string]$Repo='', [string]$ModelDirectory='', [string]$RuntimeSdk='', [ValidateRange(1,600)][int]$TimeoutSeconds=180, [switch]$RunRegressions)
$ErrorActionPreference='Stop'
if(!$Repo){$Repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path}
$Repo=[IO.Path]::GetFullPath($Repo)
if(!$ModelDirectory){$ModelDirectory=Join-Path $Repo 'build\models\pphumanseg'}
if(!$RuntimeSdk){$RuntimeSdk=Join-Path $Repo 'build\provider-deps\ort-sdk'}
$build=Join-Path $Repo ('build\custom-lighting-tests\'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $build | Out-Null
$receipt=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;actualExitCode=1;sourceHashes=@();evidenceHashes=@();processes=@();failure=$null}
$ff='C:\vcpkg\installed\x64-windows'
function Native([string]$Name,[string]$Command){
 $bat=Join-Path $build ($Name+'.cmd')
 [IO.File]::WriteAllText($bat,'@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+$Command+"`r`n"+'exit /b %ERRORLEVEL%'+"`r`n",[Text.ASCIIEncoding]::new())
 $stdout=Join-Path $build ($Name+'.stdout.log');$stderr=Join-Path $build ($Name+'.stderr.log')
 $watch=[Diagnostics.Stopwatch]::StartNew();$p=$null
 try{
  $p=Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('"'+$bat+'"')) -WorkingDirectory $Repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  $null=$p.Handle;$timedOut=!$p.WaitForExit($TimeoutSeconds*1000)
  if($timedOut){& taskkill.exe /PID $p.Id /T /F | Out-Null;$exit=124}else{$p.WaitForExit();$exit=$p.ExitCode}
  $receipt.processes+=@{label=$Name;command=$Command;actualExitCode=$exit;timedOut=$timedOut;elapsedMs=$watch.Elapsed.TotalMilliseconds}
  Get-Content -LiteralPath $stdout -Tail 40
  if((Get-Item -LiteralPath $stderr).Length){Get-Content -LiteralPath $stderr -Tail 20}
  if($exit-ne0){throw "$Name failed actualExitCode=$exit"}
 }finally{$watch.Stop();if($p){$p.Dispose()}}
}
try{
 $runtime=Join-Path $RuntimeSdk 'bin\onnxruntime.dll';$model=Join-Path $ModelDirectory 'model.onnx'
 if((Get-FileHash -LiteralPath $model).Hash-ne'552D8A984054E59B5D773D24B9B12022B22046CEB2BBC4C9AAEACEB36A9DDF24'){throw 'Pinned model hash mismatch'}
 if((Get-FileHash -LiteralPath $runtime).Hash-ne'903C92C54ACC57CAA77D44D4C856C829EB82F5CF6755B47367A8153BD3FABEB6'){throw 'Pinned runtime hash mismatch'}
 $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
 $vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
 $vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
 $dirs=@('analysisbridge','appearance','lighting','relighting','studiostack','presets','overlays','packs')
 $sources=@($dirs|ForEach-Object{Get-ChildItem -LiteralPath (Join-Path $Repo ('windows\src\effects\'+$_)) -Recurse -Filter '*.cpp'|ForEach-Object{$_.FullName}})
 $sources+=@('framepipeline.cpp','cubelut.cpp','backgroundcompositor.cpp','maskstabilizer.cpp','gpu\d3d11_context.cpp')|ForEach-Object{Join-Path $Repo ('windows\src\effects\'+$_)}
 $sources+=@(Get-ChildItem -LiteralPath (Join-Path $Repo 'windows\src\providers') -Filter '*.cpp'|ForEach-Object{$_.FullName})
 $sources+=@('effects\desktopcolorpipeline.cpp','effects\desktopstudiopipeline.cpp','video\directshowscaler.cpp','video\guipreviewscaler.cpp','video\framescaler.cpp')|ForEach-Object{Join-Path $Repo ('windows\src\'+$_)}
 $fixture=Join-Path $PSScriptRoot 'custom_lighting_tests.cpp'
 $regressions=@()
 if($RunRegressions){$regressions+=@{name='desktop';path=(Join-Path $Repo 'tests\desktopstudio\desktop_studio_tests.cpp');args=''};$regressions+=@{name='background';path=(Join-Path $PSScriptRoot 'background_workflow_tests.cpp');args=(' "'+$ModelDirectory+'" "'+$runtime+'"')}}
 $headers=@(Get-ChildItem -LiteralPath (Join-Path $Repo 'windows\src\effects') -Recurse -Filter '*.h'|ForEach-Object{$_.FullName})
 $headers+=@(Get-ChildItem -LiteralPath (Join-Path $Repo 'windows\src\providers') -Filter '*.h'|ForEach-Object{$_.FullName})
 $headers+=@(Get-ChildItem -LiteralPath (Join-Path $Repo 'windows\src\video') -Filter '*.h'|ForEach-Object{$_.FullName})
 foreach($path in @($sources+$headers+@($regressions|ForEach-Object{$_.path})+@($fixture,$PSCommandPath,$runtime,$model,(Join-Path $ModelDirectory 'manifest.json')))|Sort-Object -Unique){$receipt.sourceHashes+=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}}
 $args=@('/nologo','/c','/MP4','/O2','/openmp','/EHsc','/std:c++17','/W3','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/utf-8',('/I"'+$ff+'\include"'),('/I"'+$Repo+'\windows\src"'),('/I"'+$Repo+'"'),('/I"'+$RuntimeSdk+'\include"'),('/Fo"'+$build+'\\"'))
 $args+=@($sources|ForEach-Object{'"'+$_+'"'})
 $rsp=Join-Path $build 'compile.rsp';[IO.File]::WriteAllLines($rsp,$args,[Text.UTF8Encoding]::new($false))
 Native 'production-compile' ('cl.exe @"'+$rsp+'"')
 $objects=@(Get-ChildItem -LiteralPath $build -Filter '*.obj'|ForEach-Object{'"'+$_.FullName+'"'})-join' '
 $exe=Join-Path $build 'custom-lighting-tests.exe'
 Native 'fixture-compile' ('cl.exe /nologo /O2 /openmp /EHsc /std:c++17 /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 /I"'+$ff+'\include" /I"'+$Repo+'\windows\src" /I"'+$Repo+'" /I"'+$RuntimeSdk+'\include" "'+$fixture+'" /Fo"'+$build+'\custom_lighting_tests.obj" '+$objects+' /link /LIBPATH:"'+$ff+'\lib" ole32.lib psapi.lib d3d11.lib d3dcompiler.lib dxgi.lib avutil.lib swscale.lib avcodec.lib avformat.lib windowscodecs.lib /OUT:"'+$exe+'"')
 # Production service resolves model/runtime beside the executable, independent of CWD.
 $stagedModel=Join-Path $build 'models\pphumanseg'
 New-Item -ItemType Directory -Path $stagedModel -Force | Out-Null
 foreach($name in @('model.onnx','manifest.json')){Copy-Item -LiteralPath (Join-Path $ModelDirectory $name) -Destination (Join-Path $stagedModel $name)}
 Copy-Item -LiteralPath $runtime -Destination (Join-Path $build 'onnxruntime.dll')
 Native 'lighting-run'  ('set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'"'+$exe+'"')
 foreach($regression in $regressions){
  $regExe=Join-Path $build ($regression.name+'-regression.exe')
  $regCompile='cl.exe /nologo /O2 /openmp /EHsc /std:c++17 /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 /I"'+$ff+'\include" /I"'+$Repo+'\windows\src" /I"'+$Repo+'" /I"'+$RuntimeSdk+'\include" "'+$regression.path+'" /Fo"'+$build+'\'+$regression.name+'-regression.obj" '+$objects+' /link /LIBPATH:"'+$ff+'\lib" ole32.lib psapi.lib d3d11.lib d3dcompiler.lib dxgi.lib avutil.lib swscale.lib avcodec.lib avformat.lib windowscodecs.lib /OUT:"'+$regExe+'"'
  Native ($regression.name+'-compile') $regCompile
  Native ($regression.name+'-run') ('set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'"'+$regExe+'"'+$regression.args)
 }
 foreach($item in $receipt.sourceHashes){if((Get-FileHash -LiteralPath $item.path).Hash-ne$item.sha256){throw "Source changed during verification: $($item.path)"}}
 $receipt.regressions=$regressions.Count;$receipt.productionObjects=$sources.Count;$receipt.completed=$true;$receipt.actualExitCode=0
}catch{$receipt.failure=$_.Exception.Message;Write-Host "FAILED: $($receipt.failure)"}
finally{
 foreach($path in @(Get-ChildItem -LiteralPath $build -Recurse -File|ForEach-Object{$_.FullName})){$receipt.evidenceHashes+=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}}
 $receipt.finishedUtc=[DateTime]::UtcNow.ToString('o');$receipt|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $build 'receipt.json') -Encoding utf8
 Write-Output "CUSTOM_LIGHTING_RECEIPT $(Join-Path $build 'receipt.json')"
}
exit $receipt.actualExitCode

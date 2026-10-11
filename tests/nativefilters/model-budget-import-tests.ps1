[CmdletBinding()]
param([string]$Repo='', [string]$Downloads='LOCAL_ASSET_INPUT', [ValidateRange(1,600)][int]$TimeoutSeconds=180)
$ErrorActionPreference='Stop'
Import-Module Microsoft.PowerShell.Utility
if(!$Repo){$Repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path}
$Repo=[IO.Path]::GetFullPath($Repo)
$build=Join-Path $Repo ('build\model-budget-import-20261010\'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $build | Out-Null
$receipt=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;actualExitCode=1;sourceHashes=@();evidenceHashes=@();processes=@();proofs=@();failure=$null}
$ff='C:\vcpkg\installed\x64-windows'
function FileHash([string]$Path){
 for($attempt=0;$attempt-lt30;++$attempt){try{return (Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash}catch{if($attempt-eq29){throw};Start-Sleep -Milliseconds 100}}
}
function Native([string]$Name,[string]$Command,[int]$Deadline,[int]$Expected=0){
 $bat=Join-Path $build ($Name+'.cmd')
 [IO.File]::WriteAllText($bat,'@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+$Command+"`r`n"+'exit /b %ERRORLEVEL%'+"`r`n",[Text.ASCIIEncoding]::new())
 $stdout=Join-Path $build ($Name+'.stdout.log');$stderr=Join-Path $build ($Name+'.stderr.log')
 $watch=[Diagnostics.Stopwatch]::StartNew();$p=$null
 try{
  $p=Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('"'+$bat+'"')) -WorkingDirectory $Repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  $null=$p.Handle;$timedOut=!$p.WaitForExit($Deadline*1000)
  if($timedOut){& taskkill.exe /PID $p.Id /T /F | Out-Null;if(!$p.WaitForExit(5000)){throw "Timed-out child did not terminate: $Name"};$exit=124}else{$p.WaitForExit();$exit=$p.ExitCode}
  $receipt.processes+=@{label=$Name;command=$Command;actualExitCode=$exit;timedOut=$timedOut;deadlineSeconds=$Deadline;expectedExitCode=$Expected;elapsedMs=$watch.Elapsed.TotalMilliseconds}
  Get-Content -LiteralPath $stdout -Tail 35
  if((Get-Item -LiteralPath $stderr).Length){Get-Content -LiteralPath $stderr -Tail 12}
  if($exit-ne$Expected){throw "$Name failed actualExitCode=$exit expected=$Expected"}
 }finally{$watch.Stop();if($p){$p.Dispose()}}
}
try{
 $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
 $vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim();$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
 $sources=@('effects\overlays\face_filter_asset.cpp','effects\overlays\local_face_filter_import.cpp','effects\packs\sha256.cpp')|ForEach-Object{Join-Path $Repo ('windows\src\'+$_)}
 $headers=@('effects\overlays\accessory_mesh.h','effects\overlays\face_filter_asset.h','effects\overlays\local_face_filter_import.h','effects\overlays\model_performance.h','effects\packs\sha256.h')|ForEach-Object{Join-Path $Repo ('windows\src\'+$_)}
 $fixture=Join-Path $PSScriptRoot 'model-budget-import-tests.cpp';$inherited=Join-Path $PSScriptRoot 'local-import-diagnostics-tests.cpp'
 foreach($path in @($sources+$headers+@($PSCommandPath,$fixture,$inherited,(Join-Path $Repo 'windows\3rdparty\cgltf\cgltf.h'))+@(Get-ChildItem -LiteralPath $Downloads -File|Where-Object{$_.Name-in@('au_kingers_crown.glb','crown.glb','crown (1).glb','gold.glb','kings_crown.glb')}|ForEach-Object{$_.FullName}))|Sort-Object -Unique){$receipt.sourceHashes+=@{path=$path;sha256=(FileHash $path)}}
 $compilerArguments=@('/nologo','/c','/O2','/EHsc','/std:c++17','/W3','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/utf-8',('/I"'+$ff+'\include"'),('/I"'+$Repo+'\windows\src"'),('/I"'+$Repo+'"'),('/Fo"'+$build+'\\"'))
 $compilerArguments+=@($sources+@($fixture)|ForEach-Object{'"'+$_+'"'});$linkerArguments=@(('/LIBPATH:"'+$ff+'\lib"'),'avutil.lib','ole32.lib','windowscodecs.lib','bcrypt.lib',('/OUT:"'+$build+'\model-budget-import.exe"'))
 $rsp=Join-Path $build 'compile.rsp';[IO.File]::WriteAllLines($rsp,$compilerArguments,[Text.UTF8Encoding]::new($false))
 Native 'compile' ('cl.exe @"'+$rsp+'"') $TimeoutSeconds
 $objects=@(Get-ChildItem -LiteralPath $build -Filter '*.obj'|ForEach-Object{'"'+$_.FullName+'"'})-join' '
 Native 'link' ('link.exe /nologo '+$objects+' '+($linkerArguments-join' ')) $TimeoutSeconds
 Native 'model-budget-import' ('set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'"'+$build+'\model-budget-import.exe" "'+$build+'\fixtures" "'+$Downloads+'"') $TimeoutSeconds
 foreach($item in $receipt.sourceHashes){if((FileHash $item.path)-ne$item.sha256){throw "Source changed during verification: $($item.path)"}}
 $receipt.completed=$true;$receipt.actualExitCode=0
}catch{$receipt.failure=$_.Exception.Message;Write-Host "FAILED: $($receipt.failure)"}
finally{
 foreach($path in @(Get-ChildItem -LiteralPath $build -Recurse -File|ForEach-Object{$_.FullName})){$receipt.evidenceHashes+=@{path=$path;sha256=(FileHash $path)}}
 $receipt.finishedUtc=[DateTime]::UtcNow.ToString('o');$receipt|ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $build 'receipt.json') -Encoding utf8
 Write-Output "MODEL_BUDGET_IMPORT_RECEIPT $(Join-Path $build 'receipt.json')"
}
exit $receipt.actualExitCode


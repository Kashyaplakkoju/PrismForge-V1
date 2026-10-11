[CmdletBinding()]
param([string]$Repo='', [ValidateRange(1,600)][int]$TimeoutSeconds=180)
$ErrorActionPreference='Stop'
Import-Module Microsoft.PowerShell.Utility
if(!$Repo){$Repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path}
$Repo=[IO.Path]::GetFullPath($Repo)
$build=Join-Path $Repo ('build\native-filter-render-tests\'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $build | Out-Null
$receipt=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;actualExitCode=1;sourceHashes=@();evidenceHashes=@();processes=@();proofs=@();failure=$null}
$ff='C:\vcpkg\installed\x64-windows'
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
 $sources=@('effects\overlays\accessory_mesh.cpp','effects\overlays\face_accessory_3d.cpp','effects\overlays\face_filter_asset.cpp','effects\studiostack\gpu\d3d11_face_accessories.cpp','effects\gpu\d3d11_context.cpp')|ForEach-Object{Join-Path $Repo ('windows\src\'+$_)}
 $headers=@('effects\overlays\accessory_mesh.h','effects\overlays\face_accessory_3d.h','effects\overlays\face_filter_asset.h','effects\overlays\overlay_validation.h','effects\overlays\overlay_types.h','effects\overlays\procedural_assets.h','effects\overlays\reference_caps_mesh.inc','effects\overlays\reference_wayfarer_mesh.inc','effects\overlays\reference_glasses_collection_mesh.inc','effects\studiostack\gpu\d3d11_face_accessories.h','effects\studiostack\gpu\d3d11_studiostack_overlay.h','effects\analysisbridge\analysis_types.h','effects\presets\studio_preset.h','effects\gpu\d3d11_context.h')|ForEach-Object{Join-Path $Repo ('windows\src\'+$_)}
 $assets=Join-Path $Repo 'windows\res\face-filters';$fixture=Join-Path $PSScriptRoot 'native_filter_render_tests.cpp';$collectionFixture=Join-Path $Repo 'tests\referenceglasses\mesh_collection_tests.cpp'
 foreach($path in @($sources+$headers+@(Get-ChildItem -LiteralPath $assets -File|ForEach-Object{$_.FullName})+@($PSCommandPath,$fixture)+@(Get-ChildItem -LiteralPath (Join-Path $ff 'include\libavutil') -File|ForEach-Object{$_.FullName})+@((Join-Path $ff 'lib\avutil.lib'),(Join-Path $ff 'bin\avutil-60.dll')))|Sort-Object -Unique){$receipt.sourceHashes+=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}}
 $compilerArguments=@('/nologo','/c','/O2','/EHsc','/std:c++17','/W3','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/utf-8',('/I"'+$ff+'\include"'),('/I"'+$Repo+'\windows\src"'),('/I"'+$Repo+'"'),('/Fo"'+$build+'\\"'))
 $compilerArguments+=@($sources+@($fixture)|ForEach-Object{'"'+$_+'"'});$linkerArguments=@(('/LIBPATH:"'+$ff+'\lib"'),'avutil.lib','d3d11.lib','d3dcompiler.lib','dxgi.lib','ole32.lib','windowscodecs.lib',('/OUT:"'+$build+'\native-filter-render.exe"'))
 $rsp=Join-Path $build 'compile.rsp';[IO.File]::WriteAllLines($rsp,$compilerArguments,[Text.UTF8Encoding]::new($false))
 Native 'compile' ('cl.exe @"'+$rsp+'"') $TimeoutSeconds
 $objects=@(Get-ChildItem -LiteralPath $build -Filter '*.obj'|ForEach-Object{'"'+$_.FullName+'"'})-join' '
 Native 'link' ('link.exe /nologo '+$objects+' '+($linkerArguments-join' ')) $TimeoutSeconds
 Native 'native-filters' ('set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'"'+$build+'\native-filter-render.exe" "'+$assets+'" "'+$build+'\fixtures"') $TimeoutSeconds
 Native 'exit-proof' 'exit /b 42' 5 42
 Native 'timeout-proof' 'ping.exe -n 60 127.0.0.1 >nul' 1 124
 foreach($item in $receipt.sourceHashes){if((Get-FileHash -LiteralPath $item.path).Hash-ne$item.sha256){throw "Source changed during verification: $($item.path)"}}
 $receipt.completed=$true;$receipt.actualExitCode=0
}catch{$receipt.failure=$_.Exception.Message;Write-Host "FAILED: $($receipt.failure)"}
finally{
 foreach($path in @(Get-ChildItem -LiteralPath $build -Recurse -File|ForEach-Object{$_.FullName})){$receipt.evidenceHashes+=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}}
 $receipt.finishedUtc=[DateTime]::UtcNow.ToString('o');$receipt|ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $build 'receipt.json') -Encoding utf8
 Write-Output "NATIVE_FILTER_RENDER_RECEIPT $(Join-Path $build 'receipt.json')"
}
exit $receipt.actualExitCode

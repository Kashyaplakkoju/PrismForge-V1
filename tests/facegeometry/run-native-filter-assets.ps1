[CmdletBinding()]
param([string]$Repo='', [ValidateRange(1,600)][int]$TimeoutSeconds=120)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
if(!$Repo){$Repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path}
$Repo=[IO.Path]::GetFullPath($Repo).TrimEnd('\')
if($Repo -notmatch '^[A-Za-z]:\\' -or $Repo -match '["%\r\n]'){throw 'Invalid local repository path'}
$base=Join-Path $Repo 'build\native-filter-assets'
foreach($p in @($Repo,(Join-Path $Repo 'build'),$base)){if(Test-Path -LiteralPath $p){$i=Get-Item -LiteralPath $p -Force;if(!$i.PSIsContainer -or ($i.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Reparse evidence ancestry: $p"}}}
$build=Join-Path $base ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$receipt=[ordered]@{schemaVersion=1;startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;actualExitCode=1;sourceHashes=@();evidenceHashes=@();processes=@();failure=$null}
function Native([string]$Name,[string]$Command,[int]$Deadline){
 $bat=Join-Path $build ($Name+'.cmd')
 $nl=[Environment]::NewLine
 [IO.File]::WriteAllText($bat,'@echo off'+$nl+'call "'+$vcvars+'" >nul 2>&1'+$nl+$Command+$nl+'exit /b %ERRORLEVEL%'+$nl,[Text.ASCIIEncoding]::new())
 $stdout=Join-Path $build ($Name+'.stdout.log');$stderr=Join-Path $build ($Name+'.stderr.log')
 $p=Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('"'+$bat+'"')) -WorkingDirectory $Repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
 try{$null=$p.Handle;$timedOut=!$p.WaitForExit($Deadline*1000);if($timedOut){& taskkill.exe /PID $p.Id /T /F | Out-Null;$p.WaitForExit();$exit=124}else{$p.WaitForExit();$exit=$p.ExitCode}
 $receipt.processes+=@{label=$Name;command=$Command;actualExitCode=$exit;timedOut=$timedOut;deadlineSeconds=$Deadline}
 Get-Content -LiteralPath $stdout -Tail 14
 if((Get-Item -LiteralPath $stderr).Length){Get-Content -LiteralPath $stderr -Tail 12}
 if($exit-ne0){throw "$Name failed actualExitCode=$exit"}
 }finally{$p.Dispose()}
}
try{
 $vswhere=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
 $vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim();$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
 $src=Join-Path $Repo 'windows\src\effects\overlays\face_filter_asset.cpp'
 $fixture=Join-Path $PSScriptRoot 'native_filter_asset_tests.cpp'
 $converter=Join-Path $PSScriptRoot 'native_filter_converter_tests.py'
 $assets=Join-Path $Repo 'windows\res\face-filters'
 $paths=@($PSCommandPath,$src,$fixture,$converter,(Join-Path $Repo 'windows\src\effects\overlays\face_filter_asset.h'),(Join-Path $Repo 'windows\src\effects\overlays\accessory_mesh.h'),(Join-Path $Repo 'tools\accessories\export_face_filters.py'),(Join-Path $Repo 'tools\accessories\export_reference_caps.py'))
 $paths+=@(Get-ChildItem -LiteralPath $assets -File|ForEach-Object{$_.FullName})
 $paths+=@(Get-ChildItem -LiteralPath (Join-Path $Repo 'assets\face-filters\native-repaired-20261009') -File|Where-Object{$_.Extension -notin @('.blend1')}|ForEach-Object{$_.FullName})
 foreach($path in $paths|Sort-Object -Unique){$receipt.sourceHashes+=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}}
 Native 'compile' ('cl.exe /nologo /O2 /EHsc /std:c++17 /W3 /DNOMINMAX /utf-8 /I"'+$Repo+'\windows\src" /Fo"'+$build+'\asset.obj" /c "'+$src+'"') $TimeoutSeconds
 Native 'fixture-compile' ('cl.exe /nologo /O2 /EHsc /std:c++17 /W3 /DNOMINMAX /utf-8 /I"'+$Repo+'\windows\src" /Fo"'+$build+'\fixture.obj" /c "'+$fixture+'"') $TimeoutSeconds
 Native 'link' ('link.exe /nologo "'+$build+'\asset.obj" "'+$build+'\fixture.obj" /OUT:"'+$build+'\native-filter-assets.exe"') $TimeoutSeconds
 Native 'native-assets' ('"'+$build+'\native-filter-assets.exe" "'+$assets+'" "'+$build+'\fixtures"') $TimeoutSeconds
 Native 'converter' ('python "'+$converter+'" "'+$Repo+'\assets\face-filters\original-3d-filters-20261009"') $TimeoutSeconds
 foreach($item in $receipt.sourceHashes){if((Get-FileHash -LiteralPath $item.path).Hash-ne$item.sha256){throw "Input changed during verification: $($item.path)"}}
 $receipt.completed=$true;$receipt.actualExitCode=0
}catch{$receipt.failure=$_.Exception.Message;Write-Output "FAILED $($receipt.failure)"}
finally{
 Start-Sleep -Milliseconds 300
 $receipt.finishedUtc=[DateTime]::UtcNow.ToString('o')
 foreach($item in Get-ChildItem -LiteralPath $build -File){$receipt.evidenceHashes+=@{path=$item.FullName;sha256=(Get-FileHash -LiteralPath $item.FullName).Hash}}
 $receipt|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $build 'receipt.json') -Encoding utf8
 Write-Output "NATIVE_FILTER_ASSETS_RECEIPT $(Join-Path $build 'receipt.json')"
}
exit $receipt.actualExitCode

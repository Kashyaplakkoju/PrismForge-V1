[CmdletBinding()]
param([string]$Repo='', [int]$TimeoutSeconds=120, [string]$VcpkgRoot='C:\vcpkg')
$ErrorActionPreference='Stop'
if([string]::IsNullOrWhiteSpace($Repo)){$Repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path}
$build=Join-Path $Repo ('build\local-filter-product-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $build -Force | Out-Null
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$include=Join-Path $VcpkgRoot 'installed\x64-windows\include'
$exe=Join-Path $build 'preset-tests.exe'
$source=Join-Path $PSScriptRoot 'local-product-tests.cpp'
$preset=Join-Path $Repo 'windows\src\effects\presets\studio_preset.cpp'
$bat=Join-Path $build 'compile.cmd'
$command='cl.exe /nologo /O2 /EHsc /std:c++17 /W3 /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"'+$include+'" /I"'+$Repo+'" /I"'+$Repo+'\windows\src" "'+$source+'" "'+$preset+'" /Fo"'+$build+'\\" /link /OUT:"'+$exe+'"'
[IO.File]::WriteAllText($bat,'@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+$command+"`r`n"+'exit /b %ERRORLEVEL%'+"`r`n",[Text.ASCIIEncoding]::new())
$stdout=Join-Path $build 'compile.stdout.log';$stderr=Join-Path $build 'compile.stderr.log'
$process=Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('"'+$bat+'"')) -WorkingDirectory $Repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$null=$process.Handle
if(!$process.WaitForExit($TimeoutSeconds*1000)){& taskkill.exe /PID $process.Id /T /F | Out-Null;$compileExit=124}else{$process.WaitForExit();$compileExit=$process.ExitCode}
Get-Content $stdout -Tail 12
if((Get-Item $stderr).Length){Get-Content $stderr -Tail 12}
$runExit=$null
if($compileExit -eq 0){$output=& $exe 2>&1;$runExit=$LASTEXITCODE;$output|Set-Content (Join-Path $build 'run.log');$output}
$inputs=@($source,$preset,(Join-Path $Repo 'windows\src\effects\presets\studio_preset.h'),(Join-Path $Repo 'windows\src\effects\overlays\face_filter_asset.h'),$PSCommandPath)
[ordered]@{compileExitCode=$compileExit;runExitCode=$runExit;inputs=@($inputs|ForEach-Object{[ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}});executableSha256=if(Test-Path $exe){(Get-FileHash -LiteralPath $exe).Hash}else{$null}}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $build 'receipt.json')
Write-Output "LOCAL_FILTER_PRODUCT_EVIDENCE $build"
if($compileExit -ne 0){exit $compileExit};exit $runExit

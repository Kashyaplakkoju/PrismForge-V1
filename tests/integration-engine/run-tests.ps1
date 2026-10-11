[CmdletBinding()]
param([string]$Repo='', [int]$TimeoutSeconds=240, [switch]$SkipDesktop, [switch]$Benchmark, [string[]]$Fixtures=@(), [string]$VcpkgRoot='C:\vcpkg')
$ErrorActionPreference='Stop'
if([string]::IsNullOrWhiteSpace($Repo)){$Repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path}
$Repo=[IO.Path]::GetFullPath($Repo)
$build=Join-Path $Repo ('build\integration-engine-tests-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $build -Force | Out-Null
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$ff=Join-Path $VcpkgRoot 'installed\x64-windows'
$dirs=@('analysisbridge','appearance','lighting','relighting','studiostack','presets','overlays','packs')
$sources=@($dirs|ForEach-Object{Get-ChildItem -LiteralPath (Join-Path $Repo ('windows\src\effects\'+$_)) -Recurse -Filter '*.cpp' | ForEach-Object{$_.FullName}})
$sources+=@('framepipeline.cpp','cubelut.cpp','backgroundcompositor.cpp','maskstabilizer.cpp','gpu\d3d11_context.cpp') | ForEach-Object{Join-Path $Repo ('windows\src\effects\'+$_)}
if(!$SkipDesktop){
 $sources+=@(Get-ChildItem -LiteralPath (Join-Path $Repo 'windows\src\providers') -Filter '*.cpp' | ForEach-Object{$_.FullName})
 $sources+=@('effects\desktopcolorpipeline.cpp','effects\desktopstudiopipeline.cpp','video\directshowscaler.cpp','video\guipreviewscaler.cpp','video\framescaler.cpp') | ForEach-Object{Join-Path $Repo ('windows\src\'+$_)}
}
$hashes=@($sources|ForEach-Object{[pscustomobject]@{path=$_.Substring($Repo.Length+1);sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}})
$headers=@($dirs|ForEach-Object{Get-ChildItem -LiteralPath (Join-Path $Repo ('windows\src\effects\'+$_)) -Recurse -Filter '*.h' | ForEach-Object{$_.FullName}})
if(!$SkipDesktop){$headers+=@(Get-ChildItem -LiteralPath (Join-Path $Repo 'windows\src\providers') -Filter '*.h' | ForEach-Object{$_.FullName});$headers+=@(Join-Path $Repo 'windows\src\effects\desktopstudiopipeline.h')}
$hashes+=@($headers|ForEach-Object{[pscustomobject]@{path=$_.Substring($Repo.Length+1);sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}})
$hashes += [pscustomobject]@{path=$PSCommandPath;sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash}
$hashes | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $build 'production-source-hashes.json')
$rsp=Join-Path $build 'compile.rsp'
$args=@('/nologo','/c','/MP4','/O2','/openmp','/EHsc','/std:c++17','/W3','/wd4244','/wd4458','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/utf-8',('/I"'+$ff+'\include"'),('/I"'+$Repo+'\windows\src"'),('/I"'+$Repo+'"'),('/Fo"'+$build+'\\"'))
$args+=('/I"'+$Repo+'\build\provider-deps\ort-sdk\include"')
$args+=@($sources|ForEach-Object{'"'+$_+'"'})
[System.IO.File]::WriteAllLines($rsp,$args,[System.Text.UTF8Encoding]::new($false))
function Native([string]$Name,[string]$Command){
  $bat=Join-Path $build ($Name+'.cmd')
  [System.IO.File]::WriteAllText($bat,'@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+$Command+"`r`n"+'exit /b %ERRORLEVEL%'+"`r`n",[System.Text.ASCIIEncoding]::new())
  $stdout=Join-Path $build ($Name+'.stdout.log');$stderr=Join-Path $build ($Name+'.stderr.log')
  $p=Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('"'+$bat+'"')) -WorkingDirectory $Repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  $null=$p.Handle
  if(-not $p.WaitForExit($TimeoutSeconds*1000)) { & taskkill.exe /PID $p.Id /T /F | Out-Null; $exit=124 } else {$p.WaitForExit();$exit=$p.ExitCode}
  [ordered]@{command=$Command;actualExitCode=$exit;stdoutSha256=(Get-FileHash $stdout).Hash;stderrSha256=(Get-FileHash $stderr).Hash}|ConvertTo-Json | Set-Content (Join-Path $build ($Name+'.receipt.json'))
  Get-Content $stdout -Tail 25
  if((Get-Item $stderr).Length){Get-Content $stderr -Tail 20}
  if($exit -ne 0){throw "$Name failed actualExitCode=$exit; $build"}
}
Native 'production-compile' ('cl.exe @"'+$rsp+'"')
$objects=@(Get-ChildItem -LiteralPath $build -Filter '*.obj'|ForEach-Object{'"'+$_.FullName+'"'}) -join ' '
$testFixtures=@('desktop_looks_tests','combined_fallback_tests','conformance_tests')
if(!$SkipDesktop){$testFixtures+='desktop_studio_tests'}
if($Fixtures.Count){$testFixtures=$Fixtures}
if($Benchmark){$testFixtures=@('desktop_studio_tests')}
foreach($fixture in $testFixtures){
  $src=if($fixture -eq 'desktop_studio_tests'){Join-Path $Repo 'tests\desktopstudio\desktop_studio_tests.cpp'}else{Join-Path $Repo ('tests\integration-engine\'+$fixture+'.cpp')}
  $sourceHash=(Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
  [ordered]@{path=$src;sha256=$sourceHash}|ConvertTo-Json|Set-Content (Join-Path $build ($fixture+'.source.json'))
  $exe=Join-Path $build ($fixture+'.exe')
  $compile='cl.exe /nologo /O2 /openmp /EHsc /std:c++17 /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 /I"'+$ff+'\include" /I"'+$Repo+'\windows\src" /I"'+$Repo+'" /I"'+$Repo+'\build\provider-deps\ort-sdk\include" "'+$src+'" /Fo"'+$build+'\'+$fixture+'.obj" '+$objects+' /link /LIBPATH:"'+$ff+'\lib" ole32.lib psapi.lib d3d11.lib d3dcompiler.lib dxgi.lib avutil.lib swscale.lib avcodec.lib avformat.lib windowscodecs.lib /OUT:"'+$exe+'"'
  Native ($fixture+'-compile') $compile
  if((Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash -ne $sourceHash){throw 'Fixture source changed during compilation'}
  $run='set "PATH='+$ff+'\bin;%PATH%"'+"`r`n"+'"'+$exe+'"'
  if($Benchmark){$run+=' --bench'}
  if($fixture -eq 'conformance_tests'){$run+=' --conformance --guards --faults --switches --fanout --soak --regressions'}
  Native ($fixture+'-run') $run
}
[ordered]@{productionObjects=$sources.Count;fixtures=$testFixtures.Count;status='PASS';buildDirectory=$build;sourceHashesSha256=(Get-FileHash (Join-Path $build 'production-source-hashes.json')).Hash}|ConvertTo-Json | Set-Content (Join-Path $build 'summary.json')
Write-Output "INTEGRATION_ENGINE_PASS $build"

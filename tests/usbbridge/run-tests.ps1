param([ValidateRange(1,30)][int]$WatchdogSeconds=30,[switch]$ProbeFailure)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build=Join-Path $repo 'build\usb-bridge'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) { throw 'Existing MSVC C++ toolchain is required.' }
$testSource=Join-Path $PSScriptRoot 'bridge_tests.cpp'
$exe=Join-Path $build 'bridge_tests.exe'
$compile='call "'+$vcvars+'" >nul 2>&1 && cl.exe /nologo /O2 /EHsc /std:c++17 /W4 /DNOMINMAX /I"'+$repo+'\windows\src" "'+$testSource+'" /Fo:"'+$build+'\\" /link /OUT:"'+$exe+'"'
$compileOutput=& cmd.exe /c $compile 2>&1
$compileExit=$LASTEXITCODE
[IO.File]::WriteAllLines((Join-Path $build 'usb-bridge-build.log'),[string[]]$compileOutput)
$compileOutput | Write-Output
if ($compileExit -ne 0) { throw "USB bridge regression compilation failed: $compileExit" }
$suffix=if ($ProbeFailure) { '-failure-probe' } else { '' }
$out=Join-Path $build ('usb-bridge-tests'+$suffix+'.log')
$err=Join-Path $build ('usb-bridge-tests'+$suffix+'.stderr.log')
$startInfo=New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName=$exe
if ($ProbeFailure) { $startInfo.Arguments='--force-failure' }
$startInfo.WorkingDirectory=$build
$startInfo.UseShellExecute=$false
$startInfo.CreateNoWindow=$true
$startInfo.RedirectStandardOutput=$true
$startInfo.RedirectStandardError=$true
$process=New-Object System.Diagnostics.Process
$process.StartInfo=$startInfo
try {
    if (!$process.Start()) { throw 'Unable to start USB bridge regressions.' }
    $stdout=$process.StandardOutput.ReadToEndAsync()
    $stderr=$process.StandardError.ReadToEndAsync()
    $timedOut=!$process.WaitForExit($WatchdogSeconds*1000)
    if ($timedOut) { $process.Kill(); $process.WaitForExit() }
    $exitCode=$process.ExitCode
    $outputText=$stdout.GetAwaiter().GetResult()
    $errorText=$stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($out,$outputText)
    [IO.File]::WriteAllText($err,$errorText)
    Write-Output $outputText
    if ($errorText) { Write-Output $errorText }
    if ($timedOut) { throw "USB bridge regressions exceeded ${WatchdogSeconds}s watchdog." }
    if ($exitCode -ne 0) { throw "USB bridge regressions failed: $exitCode" }
} finally { $process.Dispose() }

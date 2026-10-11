param([string]$VcpkgRoot='C:\vcpkg',[ValidateRange(1,30)][int]$WatchdogSeconds=30,[switch]$ProbeFailure,[switch]$ProbeWatchdog)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build=Join-Path $repo 'build\frame-delivery'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$inc=Join-Path $VcpkgRoot 'installed\x64-windows\include'
$lib=Join-Path $VcpkgRoot 'installed\x64-windows\lib'
$testSource=Join-Path $PSScriptRoot 'frame_delivery_tests.cpp'
if (!(Test-Path -LiteralPath $vcvars) -or !(Test-Path -LiteralPath $inc)) { throw 'Existing MSVC and FFmpeg/vcpkg are required.' }
$exe=Join-Path $build 'frame_delivery_tests.exe'
$compile='call "'+$vcvars+'" >nul 2>&1 && cl.exe /nologo /O2 /EHsc /std:c++17 /W4 /DNOMINMAX /I"'+$inc+'" /I"'+$repo+'\windows\src" "'+$testSource+'" /Fo:"'+$build+'\\" /link /LIBPATH:"'+$lib+'" avutil.lib /OUT:"'+$exe+'"'
$compileOutput=& cmd.exe /c $compile 2>&1
$compileExit=$LASTEXITCODE
[IO.File]::WriteAllLines((Join-Path $build 'frame-delivery-build.log'),[string[]]$compileOutput)
$compileOutput | Write-Output
if ($compileExit -ne 0) { throw "Frame delivery compilation failed: $compileExit" }
$oldPath=$env:PATH
try {
    $env:PATH=(Join-Path $VcpkgRoot 'installed\x64-windows\bin')+';'+$env:PATH
    $suffix=if ($ProbeFailure) { '-failure-probe' } elseif ($ProbeWatchdog) { '-watchdog-probe' } else { '' }
    $out=Join-Path $build ('frame-delivery-tests'+$suffix+'.log')
    $err=Join-Path $build ('frame-delivery-tests'+$suffix+'.stderr.log')
    $startInfo=New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName=$exe
    if ($ProbeFailure) { $startInfo.Arguments='--force-failure' }
    elseif ($ProbeWatchdog) { $startInfo.Arguments='--watchdog-probe' }
    $startInfo.WorkingDirectory=$build
    $startInfo.UseShellExecute=$false
    $startInfo.CreateNoWindow=$true
    $startInfo.RedirectStandardOutput=$true
    $startInfo.RedirectStandardError=$true
    $process=New-Object System.Diagnostics.Process
    $process.StartInfo=$startInfo
    try {
        if (!$process.Start()) { throw 'Unable to start frame delivery regressions.' }
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
        if ($timedOut) { throw "Frame delivery regressions exceeded ${WatchdogSeconds}s watchdog." }
        if ($exitCode -ne 0) { throw "Frame delivery regressions failed: $exitCode" }
    } finally { $process.Dispose() }
} finally { $env:PATH=$oldPath }
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$DepDir = '',
    [ValidateRange(1,600)][int]$WatchdogTimeoutSec = 120,
    [switch]$SkipBenchmarks,
    [switch]$CompileOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Resolve-Path "$PSScriptRoot\..\..").Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
if (!$BuildDir) { $BuildDir = Join-Path $repoRoot 'build\provider-hardening' }
if (!$DepDir) { $DepDir = Join-Path $repoRoot 'build\provider-deps' }
$base = [IO.Path]::GetFullPath($BuildDir)
if (!$base.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw ("BuildDir must be inside this worktree. Base=[" + $base + "] Root=[" + $repoRoot + "] Default=[" + $BuildDir + "]" )
}
$runDir = Join-Path $base ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $runDir | Out-Null
$receiptPath = Join-Path $runDir 'receipt.json'
$receipt = [ordered]@{
    startedUtc=(Get-Date).ToUniversalTime().ToString('o'); runDirectory=$runDir;
    runner=$PSCommandPath; processes=@(); hashes=@(); completed=$false;
    benchmarksSkipped=[bool]$SkipBenchmarks; compileOnly=[bool]$CompileOnly;
    failure=$null; actualExitCode=1
}
function Quote-Argument([string]$value) {
    if ($value.Contains('"') -or $value.EndsWith('\')) { throw 'Unsupported quote/trailing slash in child argument.' }
    return '"' + $value + '"'
}
function Invoke-Recorded([string]$exe, [string[]]$childArgs, [string]$label, [int]$timeoutSec=$WatchdogTimeoutSec) {
    $outPath = Join-Path $runDir ($label + '.stdout.log')
    $errPath = Join-Path $runDir ($label + '.stderr.log')
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName=$exe; $psi.Arguments=($childArgs | ForEach-Object { Quote-Argument $_ }) -join ' '
    $psi.WorkingDirectory=$runDir; $psi.UseShellExecute=$false; $psi.CreateNoWindow=$true
    $psi.RedirectStandardOutput=$true; $psi.RedirectStandardError=$true
    $psi.EnvironmentVariables['PATH'] = 'C:\vcpkg\installed\x64-windows\bin;' + $env:PATH
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $process=New-Object Diagnostics.Process
    $process.StartInfo=$psi
    $exitCode=1; $timedOut=$false
    try {
        if (!$process.Start()) { throw "Unable to start $label" }
        $outTask=$process.StandardOutput.ReadToEndAsync(); $errTask=$process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit($timeoutSec*1000)) {
            $timedOut=$true; $process.Kill(); $process.WaitForExit(); $exitCode=124
        } else { $process.WaitForExit(); $exitCode=$process.ExitCode }
        $out=$outTask.GetAwaiter().GetResult(); $err=$errTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($outPath,$out); [IO.File]::WriteAllText($errPath,$err)
        if ($out -and !$label.StartsWith('imports-')) { Write-Host (($out.TrimEnd() -split "`r?`n" | Select-Object -Last 32) -join [Environment]::NewLine) }
        if ($err) { Write-Host $err.TrimEnd() }
    } finally {
        $timer.Stop(); $process.Dispose()
        $receipt.processes += [ordered]@{label=$label; command=$exe; arguments=$childArgs; actualExitCode=$exitCode;
            timedOut=$timedOut; elapsedMs=$timer.Elapsed.TotalMilliseconds; stdout=$outPath; stderr=$errPath}
    }
    Write-Host "$label actual exit=$exitCode timeout=$timedOut"
    return $exitCode
}
try {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (!$vsPath) { throw 'MSVC not found.' }
    $vcvars=Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    $dumpbin=(Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') | Sort-Object FullName | Select-Object -Last 1).FullName
    $sdk=Join-Path ([IO.Path]::GetFullPath($DepDir)) 'ort-sdk'
    $ortInc=Join-Path $sdk 'include'; $ortDll=Join-Path $sdk 'bin\onnxruntime.dll'
    if (!(Test-Path (Join-Path $ortInc 'onnxruntime_c_api.h')) -or !(Test-Path $ortDll)) {
        throw 'Pinned local ORT SDK missing. Use the existing bootstrap explicitly; this runner does not install/download.'
    }
    foreach ($dll in Get-ChildItem (Join-Path $sdk 'bin') -Filter '*.dll') { Copy-Item -LiteralPath $dll.FullName -Destination $runDir }
    $src=Join-Path $repoRoot 'windows\src'; $test=Join-Path $repoRoot 'tests\providers'
    $fixtureDir=Join-Path $runDir 'runtime-fixtures'
    if ((Invoke-Recorded (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') @('-NoProfile','-ExecutionPolicy','Bypass','-File',
        (Join-Path $test 'build-runtime-fixtures.ps1'),'-OutputDirectory',$fixtureDir,'-Vcvars',$vcvars,'-OrtInclude',$ortInc) 'build-runtime-fixtures') -ne 0) {
        throw 'Runtime failure fixture build failed.'
    }
    $common=@('providers\onnx_runner.cpp','providers\frame_preprocessor.cpp','providers\result_adapter.cpp','providers\analysis_worker.cpp','effects\analysisbridge\analysis_mailbox.cpp') | ForEach-Object { Join-Path $src $_ }
    $targets=@('provider_runtime_tests','worker_hardening_tests','runtime_loading_tests','geometry_hardening_tests','provider_integration_harness','provider_benchmarks')
    $receipt.sourceHashes=@(); foreach ($sourceFile in @(& rg --files (Join-Path $repoRoot 'windows\src\providers') (Join-Path $repoRoot 'tests\providers') (Join-Path $repoRoot 'tools\provider-deps'))) { $receipt.sourceHashes+=@{path=$sourceFile;sha256=(Get-FileHash -LiteralPath $sourceFile).Hash} }
    foreach ($target in @($targets)+@('runner_probe')) {
        $sources=@(Join-Path $test "$target.cpp")
        if ($target -ne 'runner_probe') { $sources += $common }
        foreach ($source in $sources) { if (!(Test-Path $source)) { throw "Missing required source $source" } }
        $obj=Join-Path $runDir ($target+'-obj'); New-Item -ItemType Directory -Path $obj | Out-Null
        $exe=Join-Path $runDir "$target.exe"
        $compilerArgs=@('/nologo','/O2','/EHsc','/std:c++17','/W4','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS',
            "/I$ortInc",'/IC:\vcpkg\installed\x64-windows\include',"/I$src","/I$test") + $sources
        # This local batch contains only a compiler invocation with known source/output paths.
        $compileLine='call '+(Quote-Argument $vcvars)+' >nul 2>&1'+[Environment]::NewLine+
            'cl.exe '+(($compilerArgs | ForEach-Object { Quote-Argument $_ }) -join ' ')+
            ' /Fo"'+$obj+'\\" /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" swscale.lib avutil.lib ole32.lib /OUT:"'+$exe+'"'+
            [Environment]::NewLine+'exit /b %errorlevel%'+[Environment]::NewLine
        $batch=Join-Path $runDir "compile-$target.cmd"; [IO.File]::WriteAllText($batch,$compileLine)
        if ((Invoke-Recorded $env:ComSpec @('/d','/c',$batch) "compile-$target") -ne 0) { throw "Compilation failed: $target" }
        if ($target -ne 'runner_probe') {
            if ((Invoke-Recorded $dumpbin @('/imports',$exe) "imports-$target") -ne 0) { throw 'Import audit failed.' }
            if ((Get-Content (Join-Path $runDir "imports-$target.stdout.log") -Raw) -match '(?im)^\s*onnxruntime\.dll\s*$') {
                throw "$target statically imports ORT. Optional runtime contract violated."
            }
        }
    }
    $probe=Join-Path $runDir 'runner_probe.exe'
    if ((Invoke-Recorded $probe @('exit19') 'probe-exit19') -ne 19) { throw 'Exit19 probe failed.' }
    if ((Invoke-Recorded $probe @('timeout') 'probe-watchdog' 1) -ne 124) { throw 'Watchdog probe failed.' }
    if (!$CompileOnly) {
        foreach ($target in $targets) {
            if ($target -eq 'provider_benchmarks' -and $SkipBenchmarks) { continue }
            $arguments=@()
            if ($target -eq 'provider_integration_harness') { $arguments=@('--frames','30','--verbose') }
            if ($target -eq 'runtime_loading_tests') { $arguments=@('--runtime',$ortDll,'--fixtures',$fixtureDir) }
            if ((Invoke-Recorded (Join-Path $runDir "$target.exe") $arguments $target) -ne 0) { throw "Suite failed: $target" }
        }
        # Independently run the exact asynchronous harness through the requested GPU provider too.
        if ((Invoke-Recorded (Join-Path $runDir 'provider_integration_harness.exe') @('--frames','30','--provider','dml') 'harness-dml') -ne 0) { throw 'GPU harness failed.' }
    }
    foreach ($sourceHash in $receipt.sourceHashes) { if ((Get-FileHash -LiteralPath $sourceHash.path).Hash -ne $sourceHash.sha256) { throw 'Source changed during verification; no freeze permitted.' } }
    $receipt.completed=$true; $receipt.actualExitCode=0
} catch {
    $receipt.failure=$_.Exception.Message; Write-Host "FAILED: $($receipt.failure)"
} finally {
    $files=@(& rg --files (Join-Path $repoRoot 'windows\src\providers') (Join-Path $repoRoot 'tests\providers') (Join-Path $repoRoot 'tools\provider-deps'))
    $files += @(Join-Path $repoRoot 'windows\src\effects\analysisbridge\analysis_mailbox.cpp')
    $files += @(& rg --files (Join-Path $repoRoot 'windows\src\effects\analysisbridge') | Where-Object { $_.EndsWith('.h') })
    if ($sdk -and (Test-Path $sdk)) { $files += @(Get-ChildItem $sdk -Recurse -File | ForEach-Object {$_.FullName}) }
    $files += @(Get-ChildItem $runDir -Recurse -File | ForEach-Object {$_.FullName})
    foreach ($file in ($files | Sort-Object -Unique)) {
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            $receipt.hashes += @{path=$file; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
        }
    }
    $receipt.finishedUtc=(Get-Date).ToUniversalTime().ToString('o')
    $receipt | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $receiptPath -Encoding utf8
    Write-Host "RECEIPT: $receiptPath"
}
exit $receipt.actualExitCode

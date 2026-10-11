param([string]$VcpkgRoot='C:\vcpkg',[string]$BuildDir)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build=if (!$BuildDir) { Join-Path $repo 'build\capture-review' } elseif ([IO.Path]::IsPathRooted($BuildDir)) { [IO.Path]::GetFullPath($BuildDir) } else { [IO.Path]::GetFullPath((Join-Path $repo $BuildDir)) }
New-Item -ItemType Directory -Path $build -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$inc=Join-Path $VcpkgRoot 'installed\x64-windows\include'
$lib=Join-Path $VcpkgRoot 'installed\x64-windows\lib'
$sources=@('capture/systemcamera.cpp','capture/sample_converter.cpp','capture/mf_capture.cpp','capture/dshow_capture.cpp','effects/framepipeline.cpp','video/directshowscaler.cpp','video/framescaler.cpp','rtsp/receiver.cpp') | ForEach-Object { '"'+(Join-Path $repo ('windows\src\'+$_))+'"' }
$testSource=Join-Path $repo 'tests\capture\capture_regressions.cpp'
if (!(Test-Path -LiteralPath $vcvars) -or !(Test-Path -LiteralPath $inc)) { throw 'Existing MSVC and FFmpeg/vcpkg are required.' }
$exe=Join-Path $build 'capture_regressions.exe'
$compile='call "'+$vcvars+'" >nul 2>&1 && cl.exe /nologo /MP /O2 /EHsc /std:c++17 /W3 /I"'+$inc+'" /I"'+$repo+'\windows\src" "'+$testSource+'" '+($sources -join ' ')+' /Fo:"'+$build+'\\" /link /LIBPATH:"'+$lib+'" mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib strmiids.lib ole32.lib oleaut32.lib Ws2_32.lib avutil.lib avcodec.lib avformat.lib swscale.lib /OUT:"'+$exe+'"'
$compileInfo=New-Object System.Diagnostics.ProcessStartInfo
$compileInfo.FileName=$env:ComSpec
$compileInfo.Arguments='/c '+$compile
$compileInfo.WorkingDirectory=$build
$compileInfo.UseShellExecute=$false
$compileInfo.CreateNoWindow=$true
$compileInfo.RedirectStandardOutput=$true
$compileInfo.RedirectStandardError=$true
$compiler=New-Object System.Diagnostics.Process
$compiler.StartInfo=$compileInfo
try {
    if (!$compiler.Start()) { throw 'Unable to start capture compiler.' }
    $compileStdout=$compiler.StandardOutput.ReadToEndAsync()
    $compileStderr=$compiler.StandardError.ReadToEndAsync()
    $compiler.WaitForExit()
    $compileExitCode=$compiler.ExitCode
    $compileOutput=$compileStdout.GetAwaiter().GetResult()
    $compileError=$compileStderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $build 'capture-compile.log'),$compileOutput)
    [IO.File]::WriteAllText((Join-Path $build 'capture-compile.stderr.log'),$compileError)
    [IO.File]::WriteAllText((Join-Path $build 'capture-compile.exit-code.txt'),[string]$compileExitCode)
    Write-Output $compileOutput
    if ($compileError) { Write-Output $compileError }
    if ($compileExitCode -ne 0) { throw "Capture compilation failed: $compileExitCode" }
} finally { $compiler.Dispose() }
$oldPath=$env:PATH
try {
    $env:PATH=(Join-Path $VcpkgRoot 'installed\x64-windows\bin')+';'+$env:PATH
    $out=Join-Path $build 'capture-tests.log'
    $err=Join-Path $build 'capture-tests.stderr.log'
    $startInfo=New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName=$exe
    $startInfo.WorkingDirectory=$build
    $startInfo.UseShellExecute=$false
    $startInfo.CreateNoWindow=$true
    $startInfo.RedirectStandardOutput=$true
    $startInfo.RedirectStandardError=$true
    $process=New-Object System.Diagnostics.Process
    $process.StartInfo=$startInfo
    try {
        if (!$process.Start()) { throw 'Unable to start capture regressions.' }
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(30000)) {
            $process.Kill(); $process.WaitForExit()
            throw 'Capture regressions exceeded 30s watchdog.'
        }
        $exitCode=$process.ExitCode
        [IO.File]::WriteAllText((Join-Path $build 'capture-tests.exit-code.txt'),[string]$exitCode)
        $outputText=$stdout.GetAwaiter().GetResult()
        $errorText=$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($out,$outputText)
        [IO.File]::WriteAllText($err,$errorText)
        Write-Output $outputText
        if ($errorText) { Write-Output $errorText }
        if ($exitCode -ne 0) { throw "Capture regressions failed: $exitCode" }
    } finally { $process.Dispose() }
} finally { $env:PATH=$oldPath }

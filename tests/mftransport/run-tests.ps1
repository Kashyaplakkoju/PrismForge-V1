param([string]$VcpkgRoot='C:\vcpkg')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build=Join-Path $repo 'build\mf-transport'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$inc=Join-Path $VcpkgRoot 'installed\x64-windows\include'
$lib=Join-Path $VcpkgRoot 'installed\x64-windows\lib'
$sources=@('mfsource/MFSharedMemWriter.cpp','mfsource/FrameServer.cpp','mfsource/MediaStream.cpp','effects/framepipeline.cpp','effects/cubelut.cpp','effects/desktopcolorpipeline.cpp') | ForEach-Object { '"'+(Join-Path $repo ('windows\src\'+$_))+'"' }
$testSource=Join-Path $repo 'tests\mftransport\transport_tests.cpp'
if (!(Test-Path -LiteralPath $vcvars) -or !(Test-Path -LiteralPath $inc)) { throw 'Existing MSVC and FFmpeg/vcpkg are required.' }
$exe=Join-Path $build 'mf_transport_tests.exe'
$compile='call "'+$vcvars+'" >nul 2>&1 && cl.exe /nologo /MP /O2 /EHsc /std:c++17 /W3 /DNOMINMAX /I"'+$inc+'" /I"'+$repo+'\windows\src" "'+$testSource+'" '+($sources -join ' ')+' /Fo:"'+$build+'\\" /link /LIBPATH:"'+$lib+'" avutil.lib avcodec.lib avformat.lib swscale.lib mfplat.lib mf.lib mfuuid.lib ole32.lib advapi32.lib /OUT:"'+$exe+'"'
cmd.exe /c $compile
if ($LASTEXITCODE -ne 0) { throw "MF transport compilation failed: $LASTEXITCODE" }
$oldPath=$env:PATH
try {
    $env:PATH=(Join-Path $VcpkgRoot 'installed\x64-windows\bin')+';'+$env:PATH
    $out=Join-Path $build 'mf-transport-tests.log'
    $err=Join-Path $build 'mf-transport-tests.stderr.log'
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
        if (!$process.Start()) { throw 'Unable to start MF transport regressions.' }
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(60000)) {
            $process.Kill(); $process.WaitForExit()
            throw 'MF transport regressions exceeded 60s watchdog.'
        }
        $exitCode=$process.ExitCode
        $outputText=$stdout.GetAwaiter().GetResult()
        $errorText=$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($out,$outputText)
        [IO.File]::WriteAllText($err,$errorText)
        Write-Output $outputText
        if ($errorText) { Write-Output $errorText }
        if ($exitCode -ne 0) { throw "MF transport regressions failed: $exitCode" }
    } finally { $process.Dispose() }
} finally { $env:PATH=$oldPath }

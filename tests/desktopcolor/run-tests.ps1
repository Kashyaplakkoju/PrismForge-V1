param([string]$VcpkgRoot='C:\vcpkg')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build=Join-Path $repo 'build\desktop-color'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$inc=Join-Path $VcpkgRoot 'installed\x64-windows\include'
$lib=Join-Path $VcpkgRoot 'installed\x64-windows\lib'
$sources=@('effects/framepipeline.cpp','effects/cubelut.cpp','effects/desktopcolorpipeline.cpp','video/directshowscaler.cpp','video/guipreviewscaler.cpp','video/framescaler.cpp') | ForEach-Object { '"'+(Join-Path $repo ('windows\src\'+$_))+'"' }
$testSource=Join-Path $repo 'tests\desktopcolor\desktop_color_tests.cpp'
if (!(Test-Path -LiteralPath $vcvars) -or !(Test-Path -LiteralPath $inc)) { throw 'Existing MSVC and FFmpeg/vcpkg are required.' }
$exe=Join-Path $build 'desktop_color_tests.exe'
$compile='call "'+$vcvars+'" >nul 2>&1 && cl.exe /nologo /MP /O2 /EHsc /std:c++17 /W3 /I"'+$inc+'" /I"'+$repo+'\windows\src" "'+$testSource+'" '+($sources -join ' ')+' /Fo:"'+$build+'\\" /link /LIBPATH:"'+$lib+'" avutil.lib avcodec.lib avformat.lib swscale.lib /OUT:"'+$exe+'"'
cmd.exe /c $compile
if ($LASTEXITCODE -ne 0) { throw "Desktop colour compilation failed: $LASTEXITCODE" }
$oldPath=$env:PATH
try {
    $env:PATH=(Join-Path $VcpkgRoot 'installed\x64-windows\bin')+';'+$env:PATH
    $out=Join-Path $build 'desktop-colour-tests.log'
    $err=Join-Path $build 'desktop-colour-tests.stderr.log'
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
        if (!$process.Start()) { throw 'Unable to start desktop-colour regressions.' }
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(60000)) {
            $process.Kill(); $process.WaitForExit()
            throw 'Desktop colour regressions exceeded 60s watchdog.'
        }
        $exitCode=$process.ExitCode
        $outputText=$stdout.GetAwaiter().GetResult()
        $errorText=$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($out,$outputText)
        [IO.File]::WriteAllText($err,$errorText)
        Write-Output $outputText
        if ($errorText) { Write-Output $errorText }
        if ($exitCode -ne 0) { throw "Desktop colour regressions failed: $exitCode" }
    } finally { $process.Dispose() }
} finally { $env:PATH=$oldPath }

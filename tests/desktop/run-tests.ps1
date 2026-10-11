[CmdletBinding()]
param([string]$VcpkgRoot = 'C:\vcpkg')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildDir = Join-Path $repoRoot 'build/tests-desktop'
$srcDir = Join-Path $repoRoot 'windows/src'
$wxSrc = Join-Path $repoRoot 'build/deps/wx-src/include'
$wxLib = Join-Path $repoRoot 'build/deps/wx-build/lib/vc_x64_dll'
$ffmpegRoot = Join-Path $VcpkgRoot 'installed/x64-windows'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall -or !(Test-Path -LiteralPath (Join-Path $wxLib 'wxbase33u.lib'))) { throw 'Existing MSVC and pinned Studio wxWidgets builds are required.' }
$vcvars = Join-Path $vsInstall 'VC/Auxiliary/Build/vcvars64.bat'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$testExe = Join-Path $buildDir 'desktop_looks_tests.exe'
$testSrc = Join-Path $PSScriptRoot 'desktop_looks_tests.cpp'
$pipelineSrc = Join-Path $srcDir 'effects/framepipeline.cpp'
$compile = "call `"$vcvars`" >nul 2>&1 && cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /MD /DWIN32_LEAN_AND_MEAN /DwxUSE_GUI=0 /DWXUSINGDLL /D_UNICODE /DUNICODE " +
    "/I`"$wxSrc`" /I`"$wxLib/mswu`" /I`"$srcDir`" /I`"$ffmpegRoot/include`" `"$testSrc`" `"$pipelineSrc`" /Fo:`"$buildDir\\`" /Fe:`"$testExe`" " +
    "/link /LIBPATH:`"$wxLib`" /LIBPATH:`"$ffmpegRoot/lib`" wxbase33u.lib avutil.lib avcodec.lib avformat.lib swscale.lib"
cmd.exe /c $compile
if ($LASTEXITCODE -ne 0) { throw 'Desktop look tests failed to compile.' }
$originalPath = $env:PATH
try {
    $env:PATH = "$wxLib;$ffmpegRoot/bin;$env:PATH"
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'Desktop look tests failed.' }
} finally { $env:PATH = $originalPath }

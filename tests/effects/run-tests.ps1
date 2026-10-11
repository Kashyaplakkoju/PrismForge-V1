[CmdletBinding()]
param(
    [string]$VcpkgRoot = "C:\vcpkg",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $PSScriptRoot "build"
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "PrismForge Effects: Automated Test Suite" -ForegroundColor Cyan
Write-Host "========================================================"

# 1. Discover Visual Studio via vswhere
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found at standard path: $vswhere"
    exit 1
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath -or -not (Test-Path $vsPath)) {
    Write-Error "Visual Studio installation with C++ toolset not found via vswhere."
    exit 1
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at: $vcvars"
    exit 1
}

Write-Host "[INFO] Discovered Visual Studio: $vsPath" -ForegroundColor Gray

# 2. Check vcpkg and FFmpeg dependencies
$ffmpegInc = Join-Path $VcpkgRoot "installed\x64-windows\include"
$ffmpegLib = Join-Path $VcpkgRoot "installed\x64-windows\lib"
$ffmpegBin = Join-Path $VcpkgRoot "installed\x64-windows\bin"

if (-not (Test-Path $ffmpegInc) -or -not (Test-Path $ffmpegLib)) {
    Write-Error "FFmpeg development files not found at: $VcpkgRoot\installed\x64-windows. Please verify -VcpkgRoot."
    exit 1
}

Write-Host "[INFO] Using vcpkg FFmpeg from: $VcpkgRoot" -ForegroundColor Gray

# 3. Prepare ignored build directory
$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$srcDir = Join-Path $repoRoot "windows\src"
$testSource = Join-Path $PSScriptRoot "framepipeline_tests.cpp"
$pipelineSource = Join-Path $srcDir "effects\framepipeline.cpp"
$consumerSources = @('video/framescaler.cpp', 'video/guipreviewscaler.cpp', 'video/directshowscaler.cpp', 'video/1_1scaler.cpp') | ForEach-Object { '"' + (Join-Path $srcDir $_) + '"' }
$consumerArgs = $consumerSources -join ' '


if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

$testExe = [System.IO.Path]::GetFullPath((Join-Path $BuildDir "framepipeline_tests.exe"))

# 4. Compile and Link Test Binary
Write-Host "[INFO] Compiling FramePipeline and test runner..." -ForegroundColor Yellow

$compileCmd = "call `"$vcvars`" >nul 2>&1 && cl.exe /nologo /O2 /EHsc /std:c++17 /W4 /WX- " +
              "/I`"$ffmpegInc`" /I`"$srcDir`" " +
              "`"$testSource`" `"$pipelineSource`" $consumerArgs " +
              "/Fo:`"$BuildDir\\`" " +
              "/link /LIBPATH:`"$ffmpegLib`" avutil.lib avcodec.lib avformat.lib swscale.lib " +
              "/OUT:`"$testExe`""

cmd.exe /c $compileCmd

if ($LASTEXITCODE -ne 0) {
    Write-Error "[FAIL] Compilation failed with exit code $LASTEXITCODE."
    exit $LASTEXITCODE
}

Write-Host "[SUCCESS] Compilation successful: $testExe" -ForegroundColor Green

# 5. Execute Tests
Write-Host "`n[INFO] Running tests..." -ForegroundColor Yellow
$prevPath = $env:PATH
try {
    $env:PATH = "$ffmpegBin;$env:PATH"
    & "$testExe"
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) {
        Write-Error "[FAIL] Tests failed with exit code $testExit."
        exit $testExit
    }
}
finally {
    $env:PATH = $prevPath
}

Write-Host "`n[SUCCESS] All FramePipeline verification tests passed." -ForegroundColor Green

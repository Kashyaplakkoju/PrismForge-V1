[CmdletBinding()]
param(
    [string]$VcpkgRoot = "C:\vcpkg",
    [string]$BuildDir = "",
    [string]$Suite = "--all",
    [switch]$RunRegressions,
    [string]$TestSource = "",
    [string]$ExecutableName = "conformance_tests",
    [int]$TestTimeoutSeconds = 900,
    [string[]]$ExtraSources = @()
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $PSScriptRoot "build"
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "PrismForge Milestone 9: Conformance & Performance Suite" -ForegroundColor Cyan
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

# 3. Prepare build directory
$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$srcDir = Join-Path $repoRoot "windows\src"
if ([string]::IsNullOrWhiteSpace($TestSource)) { $TestSource = Join-Path $PSScriptRoot "conformance_tests.cpp" }
$testSource = [System.IO.Path]::GetFullPath($TestSource)
if ($ExecutableName -notmatch '^[a-zA-Z0-9_-]+$' -or $TestTimeoutSeconds -lt 1) { throw "Invalid runner parameters" }

$stackSources = @(
    (Join-Path $srcDir "effects\studiostack\studio_stack.cpp"),
    (Join-Path $srcDir "effects\studiostack\gpu\d3d11_studiostack_background.cpp"),
    (Join-Path $srcDir "effects\studiostack\gpu\d3d11_studiostack_lut.cpp"),
    (Join-Path $srcDir "effects\studiostack\gpu\d3d11_studiostack_overlay.cpp"),
    (Join-Path $srcDir "effects\presets\studio_preset.cpp"),
    (Join-Path $srcDir "effects\analysisbridge\analysis_mailbox.cpp"),
    (Join-Path $srcDir "effects\analysisbridge\mask_preparer.cpp"),
    (Join-Path $srcDir "effects\lighting\studiolighting.cpp"),
    (Join-Path $srcDir "effects\appearance\appearanceprocessor.cpp"),
    (Join-Path $srcDir "effects\overlays\procedural_assets.cpp"),
    (Join-Path $srcDir "effects\overlays\overlay_renderer.cpp"),
    (Join-Path $srcDir "effects\backgroundcompositor.cpp"),
    (Join-Path $srcDir "effects\cubelut.cpp"),
    (Join-Path $srcDir "effects\maskstabilizer.cpp"),
    (Join-Path $srcDir "effects\gpu\d3d11_context.cpp"),
    (Join-Path $srcDir "effects\appearance\gpu\d3d11_appearanceprocessor.cpp"),
    (Join-Path $srcDir "effects\lighting\gpu\d3d11_studiolighting.cpp"),
    (Join-Path $srcDir "effects\packs\pack_manager.cpp"),
    (Join-Path $srcDir "effects\packs\pack_manifest.cpp"),
    (Join-Path $srcDir "effects\packs\pack_validator.cpp"),
    (Join-Path $srcDir "effects\packs\pack_preview_decoder.cpp"),
    (Join-Path $srcDir "effects\packs\sha256.cpp")
)

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

$testExe = [System.IO.Path]::GetFullPath((Join-Path $BuildDir "$ExecutableName.exe"))

# 4. Compile and Link Test Binary
Write-Host "[INFO] Compiling Conformance & Performance test runner..." -ForegroundColor Yellow

$allSources = @($testSource) + $stackSources + $ExtraSources
$sourcesStr = ($allSources | ForEach-Object { "`"$_`"" }) -join " "

$compileCmd = "call `"$vcvars`" >nul 2>&1 && cl.exe /nologo /MP /O2 /openmp /EHsc /std:c++17 /W3 /wd4244 /wd4458 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS " +
              "/I`"$ffmpegInc`" /I`"$srcDir`" /I`"$repoRoot`" " +
              "$sourcesStr " +
              "/Fo:`"$BuildDir\\`" " +
              "/link /LIBPATH:`"$ffmpegLib`" ole32.lib windowscodecs.lib bcrypt.lib psapi.lib d3d11.lib d3dcompiler.lib dxgi.lib " +
              "avutil.lib swscale.lib avcodec.lib avformat.lib " +
              "/OUT:`"$testExe`""

cmd.exe /c $compileCmd

if ($LASTEXITCODE -ne 0) {
    Write-Error "[FAIL] Compilation failed with exit code $LASTEXITCODE."
    exit $LASTEXITCODE
}

Write-Host "[SUCCESS] Compilation successful: $testExe" -ForegroundColor Green

# 5. Execute Conformance Tests
Write-Host "`n[INFO] Executing Conformance Suite with args: $Suite" -ForegroundColor Yellow
$prevPath = $env:PATH
try {
    $env:PATH = "$ffmpegBin;$env:PATH"
    $stdout = Join-Path $BuildDir "$ExecutableName.stdout.log"
    $stderr = Join-Path $BuildDir "$ExecutableName.stderr.log"
    $testProcess = Start-Process -FilePath $testExe -ArgumentList $Suite -WorkingDirectory $repoRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $testProcess.Handle # Retain OS handle so PowerShell 5.1 can retrieve the real exit code.
    if (-not $testProcess.WaitForExit($TestTimeoutSeconds * 1000)) {
        & taskkill.exe /PID $testProcess.Id /T /F | Out-Null
        Write-Host "[FAIL] Test watchdog expired after $TestTimeoutSeconds seconds. Logs: $stdout, $stderr"
        exit 124
    }
    $testProcess.WaitForExit()
    $testExit = $testProcess.ExitCode
    Get-Content -LiteralPath $stdout
    if ((Get-Item -LiteralPath $stderr).Length -gt 0) { Get-Content -LiteralPath $stderr }
    if ($null -eq $testExit) { throw "Missing test exit code" }
    if ($testExit -ne 0) {
        Write-Error "[FAIL] Conformance tests failed with exit code $testExit."
        exit $testExit
    }
}
finally {
    $env:PATH = $prevPath
}

# 6. Optional: Execute Full Regressions Across All Milestones
if ($RunRegressions) {
    Write-Host "`n========================================================" -ForegroundColor Cyan
    Write-Host "Running Read-Only Regressions Across Previous Milestones" -ForegroundColor Cyan
    Write-Host "========================================================" -ForegroundColor Cyan

    $regressionSuites = @(
        @("M1 Capture Tests", "tests\capture\run-tests.ps1"),
        @("M2 & M3 Studio Lighting Tests", "tests\lighting\run-tests.ps1"),
        @("M4 Appearance Tests", "tests\appearance\run-tests.ps1"),
        @("M5 Analysis Bridge Tests", "tests\analysisbridge\run-tests.ps1"),
        @("M6 Overlays Tests", "tests\overlays\run-tests.ps1"),
        @("M7 Effect Pack Tests", "tests\packs\run-tests.ps1"),
        @("M8 Studio Stack Tests", "tests\studiostack\run-tests.ps1")
    )

    $passedCount = 0
    $totalCount = $regressionSuites.Count

    foreach ($entry in $regressionSuites) {
        $suiteName = $entry[0]
        $suiteRel = $entry[1]
        $suitePath = Join-Path $repoRoot $suiteRel
        if (-not (Test-Path $suitePath)) {
            Write-Error "[FAIL] Missing required regression suite script: $suitePath"
            exit 1
        }

        Write-Host "`n[REGRESSION] Running $suiteName..." -ForegroundColor Yellow
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & powershell.exe -ExecutionPolicy Bypass -File "$suitePath"
        $childExit = $LASTEXITCODE
        $ErrorActionPreference = $prevEap
        if ($childExit -ne 0) {
            Write-Error "[FAIL] Regression suite $suiteName failed with exit code $childExit"
            exit $childExit
        }
        Write-Host "[PASS] $suiteName Passed!" -ForegroundColor Green
        $passedCount++
    }
    Write-Host "`n[REGRESSION SUMMARY] $passedCount / $totalCount Regression Suites PASSED!" -ForegroundColor Green
}

Write-Host "`n[SUCCESS] Milestone 9 Conformance & Performance Verification Complete!" -ForegroundColor Green
exit 0

# bootstrap_ort.ps1
# Downloads and extracts the official ONNX Runtime C/C++ SDK (v1.20.1 CPU + DirectML)
# into build/provider-deps/ort-sdk.
# Run from repository root or tools/provider-deps/.
# No persistent PATH, vcpkg, system drivers or installed software changes.

param(
    [string]$DepDir = "$PSScriptRoot\..\..\build\provider-deps"
)

$ErrorActionPreference = "Stop"

$ORT_VERSION = "1.20.1"
$PACKAGE_NAME = "Microsoft.ML.OnnxRuntime.DirectML"
$EXPECTED_SHA256 = "6763468507b7cfc777b1334b3e174c11a540ddacb7bd4354bc2e0ec89e56eec2"
$DOWNLOAD_URL = "https://www.nuget.org/api/v2/package/$PACKAGE_NAME/$ORT_VERSION"

$resolvedDepDir = [System.IO.Path]::GetFullPath($DepDir)
$zipPath = Join-Path $resolvedDepDir "ort-dml-$ORT_VERSION.zip"
$sdkDir = Join-Path $resolvedDepDir "ort-sdk"
$receiptPath = Join-Path $resolvedDepDir "ort-download.receipt.json"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "PrismForge: Bootstrapping ONNX Runtime SDK v$ORT_VERSION" -ForegroundColor Cyan
Write-Host "Package: $PACKAGE_NAME" -ForegroundColor Cyan
Write-Host "Target:  $sdkDir" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

if (-not (Test-Path $resolvedDepDir)) {
    New-Item -ItemType Directory -Path $resolvedDepDir -Force | Out-Null
}

$headerCheck = Join-Path $sdkDir "include\onnxruntime_cxx_api.h"
$libCheck = Join-Path $sdkDir "lib\onnxruntime.lib"
$dllCheck = Join-Path $sdkDir "bin\onnxruntime.dll"

if ((Test-Path $headerCheck) -and (Test-Path $libCheck) -and (Test-Path $dllCheck) -and (Test-Path $receiptPath)) {
    Write-Host "[bootstrap_ort] Verified existing SDK installation at $sdkDir" -ForegroundColor Green
    exit 0
}

# Download archive if missing or size 0
if ((-not (Test-Path $zipPath)) -or ((Get-Item $zipPath).Length -eq 0)) {
    Write-Host "[bootstrap_ort] Downloading from $DOWNLOAD_URL ..." -ForegroundColor Yellow
    # Prefer curl.exe for speed and redirect handling
    $curlPath = "curl.exe"
    $downloadSuccess = $false
    try {
        & $curlPath -L -f -s -S -o $zipPath $DOWNLOAD_URL
        if ($LASTEXITCODE -eq 0 -and (Test-Path $zipPath) -and (Get-Item $zipPath).Length -gt 0) {
            $downloadSuccess = $true
        }
    } catch {
        $downloadSuccess = $false
    }

    if (-not $downloadSuccess) {
        Write-Host "[bootstrap_ort] Falling back to Invoke-WebRequest..." -ForegroundColor Yellow
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $DOWNLOAD_URL -OutFile $zipPath -UseBasicParsing
    }
}

# Verify SHA256
$actualHash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "[bootstrap_ort] Actual SHA-256:   $actualHash"
Write-Host "[bootstrap_ort] Expected SHA-256: $EXPECTED_SHA256"

if ($actualHash -ne $EXPECTED_SHA256) {
    Write-Error "[bootstrap_ort] SHA-256 hash mismatch! Removing invalid package."
    Remove-Item $zipPath -Force
    exit 1
}
Write-Host "[bootstrap_ort] SHA-256 checksum verified." -ForegroundColor Green

# Extract package
Write-Host "[bootstrap_ort] Extracting SDK into $sdkDir ..." -ForegroundColor Yellow

$incDir = Join-Path $sdkDir "include"
$libDir = Join-Path $sdkDir "lib"
$binDir = Join-Path $sdkDir "bin"

if (-not (Test-Path $incDir)) { New-Item -ItemType Directory -Path $incDir -Force | Out-Null }
if (-not (Test-Path $libDir)) { New-Item -ItemType Directory -Path $libDir -Force | Out-Null }
if (-not (Test-Path $binDir)) { New-Item -ItemType Directory -Path $binDir -Force | Out-Null }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    foreach ($entry in $zip.Entries) {
        if ($entry.FullName.StartsWith("build/native/include/") -and -not $entry.FullName.EndsWith("/")) {
            $destFile = Join-Path $incDir ([System.IO.Path]::GetFileName($entry.FullName))
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destFile, $true)
        }
        elseif ($entry.FullName -eq "runtimes/win-x64/native/onnxruntime.lib") {
            $destFile = Join-Path $libDir "onnxruntime.lib"
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destFile, $true)
        }
        elseif ($entry.FullName -eq "runtimes/win-x64/native/onnxruntime.dll") {
            $destFile = Join-Path $binDir "onnxruntime.dll"
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destFile, $true)
        }
        elseif ($entry.FullName -in @("LICENSE", "ThirdPartyNotices.txt")) {
            $destFile = Join-Path $sdkDir $entry.FullName
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destFile, $true)
        }
    }
} finally {
    $zip.Dispose()
}

# Verify extracted layout
if ((-not (Test-Path $headerCheck)) -or (-not (Test-Path $libCheck)) -or (-not (Test-Path $dllCheck))) {
    Write-Error "[bootstrap_ort] Extraction verification failed: required files missing."
    exit 1
}

# Write reproducible receipt
$receipt = @{
    package = $PACKAGE_NAME
    version = $ORT_VERSION
    url = $DOWNLOAD_URL
    sha256 = $EXPECTED_SHA256
    downloadTimestampUtc = (Get-Date).ToUniversalTime().ToString("o")
    sdkDir = $sdkDir
    includeDir = $incDir
    libDir = $libDir
    binDir = $binDir
    licenseRetained = (Test-Path (Join-Path $sdkDir "LICENSE"))
    noticesRetained = (Test-Path (Join-Path $sdkDir "ThirdPartyNotices.txt"))
} | ConvertTo-Json -Depth 4

Set-Content -Path $receiptPath -Value $receipt -Encoding UTF8

Write-Host "[bootstrap_ort] Successfully bootstrapped ONNX Runtime SDK." -ForegroundColor Green
Write-Host "[bootstrap_ort] Receipt written to $receiptPath" -ForegroundColor DarkCyan
exit 0

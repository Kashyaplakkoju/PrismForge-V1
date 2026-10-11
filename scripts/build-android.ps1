param(
    [string]$SdkPath = $env:ANDROID_HOME,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$androidRoot = Join-Path $repoRoot 'android'
if ($SdkPath) {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkPath 'platforms/android-35/android.jar'))) {
        throw 'Android API 35 SDK not found in SdkPath.'
    }
    $sdkProperty = $SdkPath.Replace('\', '/')
    Set-Content -LiteralPath (Join-Path $androidRoot 'local.properties') -Value "sdk.dir=$sdkProperty" -Encoding ASCII
}
if (-not (Test-Path -LiteralPath (Join-Path $androidRoot 'local.properties'))) {
    throw 'Pass -SdkPath to configure the local Android SDK.'
}
# A short local path avoids Java pipe failures observed with the host temp path.
$buildTemp = Join-Path $repoRoot '.build-tmp'
New-Item -ItemType Directory -Path $buildTemp -Force | Out-Null
$originalTemp = $env:TEMP
$originalTmp = $env:TMP
$originalJavaOptions = $env:JAVA_TOOL_OPTIONS
try {
    $env:TEMP = $buildTemp
    $env:TMP = $buildTemp
    $javaTemp = $buildTemp.Replace('\', '/')
    $env:JAVA_TOOL_OPTIONS = "$originalJavaOptions -Djdk.net.unixdomain.tmpdir=$javaTemp -Djava.io.tmpdir=$javaTemp".Trim()
    Push-Location $androidRoot
    try {
        $tasks = @("assemble$Configuration")
        if ($RunTests) { $tasks += "test${Configuration}UnitTest" }
        & .\gradlew.bat @tasks --no-daemon --console=plain
        if ($LASTEXITCODE -ne 0) { throw 'Android build failed.' }
    } finally { Pop-Location }
    if ($Configuration -eq 'Debug') {
        $apkDir = Join-Path $repoRoot 'dist/apk'
        New-Item -ItemType Directory -Path $apkDir -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $androidRoot 'app/build/outputs/apk/debug/app-debug.apk') -Destination (Join-Path $apkDir 'prismforge-companion-debug.apk')
    } else {
        Write-Output 'Release APK is unsigned until a release signing configuration is supplied. Do not distribute it as a signed release.'
    }
} finally {
    $env:TEMP = $originalTemp
    $env:TMP = $originalTmp
    $env:JAVA_TOOL_OPTIONS = $originalJavaOptions
}

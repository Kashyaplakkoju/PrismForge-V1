[CmdletBinding()]
param(
    [string]$Repo = '',
    [string]$RuntimeDll = '',
    [string]$ModelFile = '',
    [string]$TopologyFile = '',
    [string]$FixtureImage = '',
    [switch]$RunLighting,
    [string]$VcpkgRoot = 'C:\vcpkg',
    [ValidateRange(1, 600)][int]$CompileTimeoutSeconds = 120,
    [ValidateRange(1, 600)][int]$TestTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
if ([string]::IsNullOrWhiteSpace($Repo)) {
    $Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
}
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$Repo = [IO.Path]::GetFullPath($Repo).TrimEnd('\')
if ($Repo -notmatch '^[A-Za-z]:\\' -or $Repo -match '["%\r\n]') {
    throw 'The repository must be a local drive path without batch expansion characters.'
}

# A fresh directory is never reused. Reject existing junctions in its ancestry so
# evidence creation cannot escape this checkout through a redirected build folder.
$evidenceRoot = [IO.Path]::GetFullPath((Join-Path $Repo 'build\facegeometry'))
if (-not $evidenceRoot.StartsWith($Repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Evidence target is outside the repository.'
}
foreach ($directory in @($Repo, (Join-Path $Repo 'build'), $evidenceRoot)) {
    if (Test-Path -LiteralPath $directory) {
        $item = Get-Item -LiteralPath $directory -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Evidence ancestry is not an ordinary directory: $directory"
        }
    }
}
[IO.Directory]::CreateDirectory($evidenceRoot) | Out-Null
$runDirectory = Join-Path $evidenceRoot ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $runDirectory) { throw 'A fresh evidence directory already exists.' }
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null
$snapshotRoot = Join-Path $runDirectory 'snapshot'
[IO.Directory]::CreateDirectory($snapshotRoot) | Out-Null
$receipt = [ordered]@{
    schemaVersion = 1
    startedUtc = [DateTime]::UtcNow.ToString('o')
    finishedUtc = $null
    completed = $false
    runnerExitCode = 1
    actualExitCode = $null
    fixtureSupplied = -not [string]::IsNullOrWhiteSpace($FixtureImage)
    lightingRequested = [bool]$RunLighting
    evidenceDirectory = $runDirectory
    sourceHashes = @()
    assetHashes = @()
    dependencyHashes = @()
    processes = @()
    evidenceHashes = @()
    failure = $null
    inputVerificationFailure = $null
}
$inputRecords = [Collections.Generic.List[object]]::new()
$receiptPath = Join-Path $runDirectory 'receipt.json'

function Write-NewText([string]$Path, [string]$Content) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Content)
        $stream.Write($bytes, 0, $bytes.Length)
    } finally { $stream.Dispose() }
}

function Snapshot-Input([string]$SourcePath, [string]$RelativePath, [string]$Kind, [string]$ExpectedHash = '') {
    $resolved = (Resolve-Path -LiteralPath $SourcePath).Path
    $source = Get-Item -LiteralPath $resolved
    if ($source.PSIsContainer) { throw "Input is not a regular file: $resolved" }
    $hash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
    if ($ExpectedHash -and $hash -ne $ExpectedHash) { throw "Pinned $Kind digest mismatch: $resolved" }
    $destination = [IO.Path]::GetFullPath((Join-Path $snapshotRoot $RelativePath))
    if (-not $destination.StartsWith($snapshotRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Snapshot target escapes its evidence directory.'
    }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    [IO.File]::Copy($resolved, $destination, $false)
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) {
        throw "Input changed while copying: $resolved"
    }
    $record = [ordered]@{path=$resolved; snapshot=$destination; bytes=$source.Length; sha256=$hash}
    $inputRecords.Add($record)
    switch ($Kind) {
        'source' { $receipt.sourceHashes += $record }
        'dependency' { $receipt.dependencyHashes += $record }
        default { $receipt.assetHashes += $record }
    }
    return $destination
}

# Native argument quoting follows the Windows CRT convention. No fixture or
# asset path is interpolated into a shell command.
function Quote-NativeArgument([string]$Value) {
    if ($Value.IndexOf([char]0) -ge 0 -or $Value -match '[\r\n]') { throw 'Invalid native argument.' }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Invoke-BoundedProcess([string]$Label, [string]$FilePath, [string]$Arguments,
                               [string]$WorkingDirectory, [int]$TimeoutSeconds) {
    $stdoutPath = Join-Path $runDirectory ($Label + '.stdout.log')
    $stderrPath = Join-Path $runDirectory ($Label + '.stderr.log')
    if ((Test-Path -LiteralPath $stdoutPath) -or (Test-Path -LiteralPath $stderrPath)) {
        throw "Process evidence would be overwritten: $Label"
    }
    $record = [ordered]@{
        label=$Label; file=$FilePath; arguments=$Arguments; workingDirectory=$WorkingDirectory
        startedUtc=[DateTime]::UtcNow.ToString('o'); elapsedMs=0.0; pid=$null
        timeoutSeconds=$TimeoutSeconds; timedOut=$false; actualExitCode=$null
        runnerExitCode=1; stdout=$stdoutPath; stderr=$stderrPath; launchFailure=$null
    }
    $receipt.processes += $record
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = $null
    try {
        $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -WorkingDirectory $WorkingDirectory `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        $null = $process.Handle # Retain the OS handle for the real exit code on Windows PowerShell 5.1.
        $record.pid = $process.Id
        $record.timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($record.timedOut) {
            $killOut = Join-Path $runDirectory ($Label + '.watchdog.stdout.log')
            $killErr = Join-Path $runDirectory ($Label + '.watchdog.stderr.log')
            $killer = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\taskkill.exe') `
                -ArgumentList ('/PID ' + $process.Id + ' /T /F') -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $killOut -RedirectStandardError $killErr
            try {
                $null = $killer.Handle
                if (-not $killer.WaitForExit(10000)) { $killer.Kill(); $null = $killer.WaitForExit(5000) }
                if ($killer.HasExited) { $record.watchdogExitCode = $killer.ExitCode }
            } finally { $killer.Dispose() }
            $null = $process.WaitForExit(10000)
            if ($process.HasExited) { $record.actualExitCode = $process.ExitCode }
            $record.runnerExitCode = 124
            $receipt.runnerExitCode = 124
            $receipt.actualExitCode = $record.actualExitCode
            throw "$Label exceeded its $TimeoutSeconds second watchdog; raw exit is recorded separately."
        }
        $process.WaitForExit()
        $record.actualExitCode = $process.ExitCode
        if ($null -eq $record.actualExitCode) { throw "$Label did not expose a native exit code." }
        $record.runnerExitCode = $record.actualExitCode
        if ($record.actualExitCode -ne 0) {
            $receipt.runnerExitCode = $record.actualExitCode
            $receipt.actualExitCode = $record.actualExitCode
            throw "$Label failed with actual native exit $($record.actualExitCode)."
        }
    } catch {
        if (-not $process) { $record.launchFailure = $_.Exception.Message }
        throw
    } finally {
        $watch.Stop()
        $record.elapsedMs = $watch.Elapsed.TotalMilliseconds
        if ($process) { $process.Dispose() }
        if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath -Tail 60 | Write-Host }
        if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath -Tail 30 | Write-Host }
    }
}

function Compile-Fixture([string]$Label, [string[]]$Sources, [string[]]$Libraries, [string[]]$Includes) {
    $objectDirectory = Join-Path $runDirectory ($Label + '-objects')
    [IO.Directory]::CreateDirectory($objectDirectory) | Out-Null
    $executable = Join-Path $runDirectory ($Label + '.exe')
    $responsePath = Join-Path $runDirectory ($Label + '.rsp')
    $compilerArguments = @('/nologo', '/Bv', '/O2', '/EHsc', '/std:c++17', '/W4', '/DNOMINMAX',
              '/D_CRT_SECURE_NO_WARNINGS', '/utf-8', '/MD')
    $compilerArguments += @($Includes | ForEach-Object { '/I' + (Quote-NativeArgument $_) })
    $compilerArguments += @($Sources | ForEach-Object { Quote-NativeArgument (Join-Path $snapshotRoot $_) })
    $compilerArguments += '/link'
    $compilerArguments += $Libraries
    $compilerArguments += '/OUT:' + (Quote-NativeArgument $executable)
    Write-NewText $responsePath (($compilerArguments -join ' ') + "`r`n")
    $batchPath = Join-Path $runDirectory ($Label + '-compile.cmd')
    $batch = '@echo off' + "`r`n" + 'setlocal DisableDelayedExpansion' + "`r`n" +
        'call "' + $vcvars + '" >nul 2>&1' + "`r`n" + 'if errorlevel 1 exit /b %ERRORLEVEL%' + "`r`n" +
        'cl.exe @"' + $responsePath + '"' + "`r`n" + 'exit /b %ERRORLEVEL%' + "`r`n"
    Write-NewText $batchPath $batch
    Invoke-BoundedProcess ($Label + '-compile') $env:ComSpec ('/d /c "' + $batchPath + '"') $objectDirectory $CompileTimeoutSeconds
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Compiler did not produce $executable" }
    return $executable
}

try {
    if ($RunLighting -and [string]::IsNullOrWhiteSpace($FixtureImage)) {
        throw '-RunLighting requires a supplied face fixture through -FixtureImage.'
    }
    if (-not $RuntimeDll) { $RuntimeDll = Join-Path $Repo 'build\face-native-deps\mediapipe-0.10.32\mediapipe\tasks\c\libmediapipe.dll' }
    if (-not $ModelFile) { $ModelFile = Join-Path $Repo 'build\face-native-deps\face-landmarker-v1\face_landmarker.task' }
    if (-not $TopologyFile) { $TopologyFile = Join-Path $Repo 'build\face-native-deps\face-landmarker-v1\canonical_face_model.obj' }
    $runtimeSnapshot = Snapshot-Input $RuntimeDll 'assets\libmediapipe.dll' 'asset' '6DBB21A391CF79BEAD3D82EFB00D02B3638435340C950545042B83194A5B87E7'
    $modelSnapshot = Snapshot-Input $ModelFile 'assets\face_landmarker.task' 'asset' '64184E229B263107BC2B804C6625DB1341FF2BB731874B0BCC2FE6544E0BC9FF'
    $topologySnapshot = Snapshot-Input $TopologyFile 'assets\canonical_face_model.obj' 'asset' '8BAC80443397E113F41A8B565EA72C59390BC031D9DEFAB289DBA7BC0C54E618'
    $fixtureSnapshot = ''
    if ($FixtureImage) {
        $fixtureSnapshot = Snapshot-Input $FixtureImage ('assets\fixture' + [IO.Path]::GetExtension($FixtureImage)) 'asset'
    }
    $geometrySources = @('tests\facegeometry\face_geometry_tests.cpp', 'windows\src\providers\face_geometry.cpp')
    $runtimeSources = @('tests\facegeometry\face_runtime_tests.cpp', 'windows\src\providers\face_landmarker_runtime.cpp', 'windows\src\providers\face_geometry.cpp')
    $sourcePaths = @($geometrySources + $runtimeSources + @(
        'tests\facegeometry\run-tests.ps1', 'windows\src\providers\face_geometry.h',
        'windows\src\providers\face_landmarker_runtime.h', 'windows\src\providers\face_types.h',
        'windows\src\effects\analysisbridge\analysis_types.h'))
    $lightingSources = @()
    if ($RunLighting) {
        $lightingSources = @('tests\facegeometry\face_lighting_tests.cpp', 'windows\src\providers\face_geometry.cpp',
            'windows\src\providers\face_landmarker_runtime.cpp', 'windows\src\effects\lighting\studiolighting.cpp',
            'windows\src\effects\lighting\gpu\d3d11_studiolighting.cpp', 'windows\src\effects\relighting\approximate_shadows.cpp',
            'windows\src\effects\relighting\gpu\d3d11_approximate_shadows.cpp')
        $sourcePaths += $lightingSources
        foreach ($subdirectory in @('effects\lighting', 'effects\relighting')) {
            $headerRoot = Join-Path $Repo ('windows\src\' + $subdirectory)
            $sourcePaths += @(Get-ChildItem -LiteralPath $headerRoot -Filter '*.h' -Recurse | ForEach-Object { $_.FullName.Substring($Repo.Length + 1) })
        }
    }
    foreach ($relativePath in @($sourcePaths | Sort-Object -Unique)) {
        $null = Snapshot-Input (Join-Path $Repo $relativePath) $relativePath 'source'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    Invoke-BoundedProcess 'toolchain-discovery' $vswhere '-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath' $runDirectory 30
    $vsPath = (Get-Content -LiteralPath (Join-Path $runDirectory 'toolchain-discovery.stdout.log') -Raw).Trim()
    if (-not $vsPath -or $vsPath -match '["%\r\n]') { throw 'Could not resolve one safe MSVC installation path.' }
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw 'The x64 MSVC environment script is unavailable.' }
    $receipt.toolchain = @{installation=$vsPath; vcvars=$vcvars; vcvarsSha256=(Get-FileHash -LiteralPath $vcvars -Algorithm SHA256).Hash}
    $includes = @((Join-Path $snapshotRoot 'windows\src'))
    $geometryExe = Compile-Fixture 'face-geometry-tests' $geometrySources @() $includes
    $runtimeExe = Compile-Fixture 'face-runtime-tests' $runtimeSources @('bcrypt.lib', 'windowscodecs.lib', 'ole32.lib') $includes
    Invoke-BoundedProcess 'face-geometry-run' $geometryExe (Quote-NativeArgument $topologySnapshot) $runDirectory $TestTimeoutSeconds
    $runtimeArguments = @($runtimeSnapshot, $modelSnapshot, $topologySnapshot)
    if ($fixtureSnapshot) { $runtimeArguments += $fixtureSnapshot }
    $argumentText = (@($runtimeArguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' ')
    Invoke-BoundedProcess 'face-runtime-run' $runtimeExe $argumentText $runDirectory $TestTimeoutSeconds
    if ($RunLighting) {
        $ffmpegRoot = Join-Path $VcpkgRoot 'installed\x64-windows'
        foreach ($header in @(Get-ChildItem -LiteralPath (Join-Path $ffmpegRoot 'include\libavutil') -Filter '*.h' -Recurse)) {
            $null = Snapshot-Input $header.FullName ('dependency\include\libavutil\' + $header.FullName.Substring((Join-Path $ffmpegRoot 'include\libavutil').Length + 1)) 'dependency'
        }
        $avutilLibrary = Snapshot-Input (Join-Path $ffmpegRoot 'lib\avutil.lib') 'dependency\lib\avutil.lib' 'dependency'
        $avutilDll = @(Get-ChildItem -LiteralPath (Join-Path $ffmpegRoot 'bin') -Filter 'avutil-*.dll')
        if ($avutilDll.Count -ne 1) { throw 'Expected exactly one release avutil runtime DLL.' }
        $avutilSnapshot = Snapshot-Input $avutilDll[0].FullName ('dependency\bin\' + $avutilDll[0].Name) 'dependency'
        [IO.File]::Copy($avutilSnapshot, (Join-Path $runDirectory $avutilDll[0].Name), $false)
        $lightingIncludes = $includes + @((Join-Path $snapshotRoot 'dependency\include'))
        $lightingLibraries = @((Quote-NativeArgument $avutilLibrary), 'd3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'bcrypt.lib', 'ole32.lib', 'windowscodecs.lib')
        $lightingExe = Compile-Fixture 'face-lighting-tests' $lightingSources $lightingLibraries $lightingIncludes
        Invoke-BoundedProcess 'face-lighting-run' $lightingExe $argumentText $runDirectory $TestTimeoutSeconds
    }
    $receipt.completed = $true
    $receipt.runnerExitCode = 0
    $receipt.actualExitCode = 0
} catch {
    $receipt.failure = $_.Exception.Message
    Write-Host ('FAILED: ' + $receipt.failure)
} finally {
    try {
        foreach ($record in $inputRecords) {
            if ((Get-FileHash -LiteralPath $record.path -Algorithm SHA256).Hash -ne $record.sha256 -or
                (Get-FileHash -LiteralPath $record.snapshot -Algorithm SHA256).Hash -ne $record.sha256) {
                throw "Original or snapshot changed during verification: $($record.path)"
            }
        }
    } catch {
        $receipt.inputVerificationFailure = $_.Exception.Message
        $receipt.completed = $false
        if ($receipt.runnerExitCode -eq 0) { $receipt.runnerExitCode = 1 }
        Write-Host ('FAILED: ' + $receipt.inputVerificationFailure)
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $runDirectory -File -Recurse | Sort-Object FullName)) {
        $receipt.evidenceHashes += @{path=$file.FullName; bytes=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
    }
    $receipt.finishedUtc = [DateTime]::UtcNow.ToString('o')
    Write-NewText $receiptPath (($receipt | ConvertTo-Json -Depth 12) + "`r`n")
    Write-Output ('FACE_GEOMETRY_RECEIPT ' + $receiptPath)
}
exit $receipt.runnerExitCode

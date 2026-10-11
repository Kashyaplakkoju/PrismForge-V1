param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$Vcvars,
    [Parameter(Mandatory=$true)][string]$OrtInclude
)
$ErrorActionPreference = 'Stop'
$fixtureOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
$sourcePath = Join-Path $PSScriptRoot 'runtime_loading_tests.cpp'
New-Item -ItemType Directory -Path $fixtureOutput -Force | Out-Null
$dependencyOutput = Join-Path $fixtureOutput 'dependency-build'
New-Item -ItemType Directory -Path $dependencyOutput -Force | Out-Null
$fixtureReceipts = @()
function Build-Fixture([string]$Name, [string]$Define, [string]$DllDirectory, [string]$ExtraLink = '') {
    $dllPath = Join-Path $DllDirectory "$Name.dll"
    $libraryPath = Join-Path $fixtureOutput "$Name.lib"
    $objectPath = Join-Path $fixtureOutput "$Name.obj"
    $batchPath = Join-Path $fixtureOutput "$Name.compile.cmd"
    $stdoutPath = Join-Path $fixtureOutput "$Name.compile.stdout.log"
    $stderrPath = Join-Path $fixtureOutput "$Name.compile.stderr.log"
    $compileCommand = "cl.exe /nologo /LD /O2 /EHsc /std:c++17 /W4 /DNOMINMAX /D$Define /I`"$OrtInclude`" `"$sourcePath`" /Fo`"$objectPath`" /link /OUT:`"$dllPath`" /IMPLIB:`"$libraryPath`" $ExtraLink"
    $batch = "@call `"$Vcvars`" >nul 2>&1`r`n@" + $compileCommand + "`r`n@exit /b %ERRORLEVEL%`r`n"
    [System.IO.File]::WriteAllText($batchPath, $batch, [System.Text.ASCIIEncoding]::new())
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $env:ComSpec
    $startInfo.Arguments = '/d /s /c ""' + $batchPath + '""'
    $startInfo.WorkingDirectory = $fixtureOutput
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = -not $process.WaitForExit(60000)
    if ($timedOut) { $process.Kill(); $process.WaitForExit(); $actualExit = 124 }
    else { $process.WaitForExit(); $actualExit = $process.ExitCode }
    $clock.Stop()
    [System.IO.File]::WriteAllText($stdoutPath, $stdout.Result, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($stderrPath, $stderr.Result, [System.Text.UTF8Encoding]::new($false))
    $receipt = [ordered]@{ name=$Name; command=$compileCommand; exit=$actualExit; timedOut=$timedOut; elapsedMs=$clock.Elapsed.TotalMilliseconds; stdout=$stdoutPath; stderr=$stderrPath; sourceSha256=(Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash; scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash; batchSha256=(Get-FileHash -LiteralPath $batchPath -Algorithm SHA256).Hash; dllPath=$dllPath; dllSha256=$null }
    if (Test-Path -LiteralPath $dllPath) { $receipt.dllSha256=(Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash }
    $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $fixtureOutput "$Name.compile.receipt.json") -Encoding UTF8
    if ($actualExit -ne 0) { Write-Host $stdout.Result; Write-Host $stderr.Result; throw "Fixture $Name compile exit $actualExit" }
    $script:fixtureReceipts += $receipt
}
Build-Fixture 'wrong-export' 'PRISM_RUNTIME_WRONG_EXPORT' $fixtureOutput
Build-Fixture 'wrong-version' 'PRISM_RUNTIME_WRONG_VERSION' $fixtureOutput
Build-Fixture 'missing-dependency' 'PRISM_RUNTIME_MISSING_DEPENDENCY' $dependencyOutput
$dependencyLibrary = Join-Path $fixtureOutput 'missing-dependency.lib'
Build-Fixture 'requires-dependency' 'PRISM_RUNTIME_REQUIRES_DEPENDENCY' $fixtureOutput ('"' + $dependencyLibrary + '"')
$fixtureReceipts | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $fixtureOutput 'fixtures.receipt.json') -Encoding UTF8
Write-Output "[FIXTURES] Four original loader fixtures compiled; required dependency is isolated in dependency-build, outside scoped search."
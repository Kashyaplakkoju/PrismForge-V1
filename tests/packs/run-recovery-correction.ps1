param([ValidateRange(2,600)][int]$TimeoutSeconds = 90)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString().Substring(0,8)
$run = Join-Path $repo ('build\pack-recovery-' + $runId)
if (Test-Path -LiteralPath $run) { throw 'Run directory must be fresh' }
New-Item -ItemType Directory -Path $run | Out-Null
$inputs = @(
    'tests/packs/run-recovery-correction.ps1',
    'tests/packs/pack_recovery_correction_tests.cpp',
    'windows/src/effects/packs/pack_manager.h',
    'windows/src/effects/packs/pack_manager.cpp',
    'windows/src/effects/packs/pack_manifest.h',
    'windows/src/effects/packs/pack_manifest.cpp',
    'windows/src/effects/packs/pack_validator.h', 'windows/src/effects/packs/pack_preview_decoder.h',
    'windows/src/effects/packs/pack_validator.cpp', 'windows/src/effects/packs/pack_preview_decoder.cpp',
    'windows/src/effects/packs/sha256.h',
    'windows/src/effects/packs/sha256.cpp'
)
$pre = [ordered]@{}
foreach ($relative in $inputs) {
    $inputPath = Join-Path $repo $relative
    $pre[$relative] = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
    $snapshot = Join-Path $run ('sources\' + $relative)
    New-Item -ItemType Directory -Path (Split-Path -Parent $snapshot) -Force | Out-Null
    Copy-Item -LiteralPath $inputPath -Destination $snapshot
}
$receipt = [ordered]@{
    runId = $runId
    startedUtc = [DateTime]::UtcNow.ToString('o')
    sourceHashes = $pre
    processes = @()
    completed = $false
    successful = $false
}
$receiptPath = Join-Path $run 'receipt.json'
$script:children = @()

function Invoke-Bounded([string]$Name, [string]$Executable, [string]$Arguments, [int]$Deadline) {
    $out = Join-Path $run ($Name + '.stdout.log')
    $err = Join-Path $run ($Name + '.stderr.log')
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $proc.Handle
    $timedOut = -not $proc.WaitForExit($Deadline * 1000)
    if ($timedOut) {
        & taskkill.exe /PID $proc.Id /T /F | Out-Null
        $proc.WaitForExit()
        $exit = 124
    } else {
        $proc.WaitForExit()
        $exit = $proc.ExitCode
    }
    $clock.Stop()
    $entry = [ordered]@{
        name = $Name
        actualExit = $proc.ExitCode
        reportedExit = $exit
        timedOut = $timedOut
        elapsedSeconds = $clock.Elapsed.TotalSeconds
        stdout = $out
        stderr = $err
    }
    $script:children += $entry
    $receipt.processes = $script:children
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    return $entry
}

$overall = 1
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Installed C++ toolchain unavailable' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $installed = 'C:\vcpkg\installed\x64-windows'
    $env:PATH = (Join-Path $installed 'bin') + ';' + $env:PATH
    $sources = @(
        'tests/packs/pack_recovery_correction_tests.cpp',
        'windows/src/effects/packs/pack_manager.cpp',
        'windows/src/effects/packs/pack_manifest.cpp',
        'windows/src/effects/packs/pack_validator.cpp', 'windows/src/effects/packs/pack_preview_decoder.cpp',
        'windows/src/effects/packs/sha256.cpp'
    )
    $quotedSources = ($sources | ForEach-Object { '"' + (Join-Path $repo $_) + '"' }) -join ' '
    $exe = Join-Path $run 'pack_recovery_correction_tests.exe'
    $cmdFile = Join-Path $run 'compile.cmd'
    $compile = '@echo off' + "`r`n" +
        'call "' + $vcvars + '" >nul 2>&1' + "`r`n" +
        'if errorlevel 1 exit /b %errorlevel%' + "`r`n" +
        'cl.exe /nologo /O2 /EHsc /std:c++17 /W4 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"' + (Join-Path $installed 'include') + '" /I"' + $repo + '" ' + $quotedSources +
        ' /link /LIBPATH:"' + (Join-Path $installed 'lib') + '" psapi.lib ole32.lib windowscodecs.lib bcrypt.lib /OUT:"' + $exe + '"' + "`r`nexit /b %errorlevel%`r`n"
    [IO.File]::WriteAllText($cmdFile, $compile, [Text.Encoding]::ASCII)

    $child = Invoke-Bounded 'compile' 'cmd.exe' ('/d /c ""' + $cmdFile + '""') $TimeoutSeconds
    if ($child.reportedExit -ne 0) {
        Get-Content -LiteralPath $child.stdout
        Get-Content -LiteralPath $child.stderr
        throw ('Compiler exit ' + $child.reportedExit)
    }
    $receipt.binarySha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash

    $child = Invoke-Bounded 'tests' $exe ('"' + (Join-Path $run 'fixtures') + '"') $TimeoutSeconds
    Get-Content -LiteralPath $child.stdout
    if ($child.reportedExit -ne 0) {
        Get-Content -LiteralPath $child.stderr
        throw ('Tests exit ' + $child.reportedExit)
    }

    $child = Invoke-Bounded 'nonzero-proof' $exe '--exit-proof' 5
    if ($child.actualExit -ne 42 -or $child.timedOut) {
        throw 'Nonzero proof must record the actual child exit 42'
    }

    $child = Invoke-Bounded 'watchdog-proof' $exe '--hang-proof' 2
    if (-not $child.timedOut -or $child.reportedExit -ne 124) {
        throw 'Watchdog proof must terminate scoped hung child and report 124'
    }

    $overall = 0
} catch {
    $receipt.error = $_.Exception.Message
    Write-Host ('FAIL: ' + $receipt.error) -ForegroundColor Red
} finally {
    $post = [ordered]@{}
    $unchanged = $true
    foreach ($relative in $inputs) {
        $post[$relative] = (Get-FileHash -LiteralPath (Join-Path $repo $relative) -Algorithm SHA256).Hash
        if ($pre[$relative] -ne $post[$relative]) { $unchanged = $false }
    }
    $receipt.sourceHashesAfter = $post
    $receipt.sourcesUnchanged = $unchanged
    if (-not $unchanged) { $overall = 1 }

    $evidence = [ordered]@{}
    foreach ($c in $script:children) {
        foreach ($p in @($c.stdout, $c.stderr)) {
            if (Test-Path -LiteralPath $p) {
                $evidence[$p] = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
            }
        }
    }
    $receipt.evidenceHashes = $evidence
    $receipt.completed = $true
    $receipt.successful = ($overall -eq 0)
    $receipt.actualRunnerExit = $overall
    $receipt.completedUtc = [DateTime]::UtcNow.ToString('o')
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    Write-Host ('Receipt: ' + $receiptPath)
}
exit $overall

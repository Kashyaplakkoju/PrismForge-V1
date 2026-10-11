param([ValidateRange(5,600)][int]$TimeoutSeconds=120)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runId=(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString().Substring(0,8)
$run=Join-Path $repo ('build\pack-codex-transactions-'+$runId)
New-Item -ItemType Directory -Path $run | Out-Null
$receiptPath=Join-Path $run 'receipt.json'
$receipt=[ordered]@{runId=$runId;startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;successful=$false;processes=@();sourceHashes=@{};dependencies=@{}}
function Save-Receipt {$receipt|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $receiptPath -Encoding UTF8}
Save-Receipt
$children=@();$pre=[ordered]@{};$inputs=@();$overall=1
function Invoke-Owned([string]$Name,[string]$Executable,[string]$Arguments,[int]$Seconds) {
    $stdout=Join-Path $run ($Name+'.stdout.log');$stderr=Join-Path $run ($Name+'.stderr.log')
    $entry=[ordered]@{name=$Name;executable=$Executable;arguments=$Arguments;deadlineSeconds=$Seconds;stdout=$stdout;stderr=$stderr;startedUtc=[DateTime]::UtcNow.ToString('o');pid=$null;actualExit=$null;reportedExit=$null;timedOut=$false;killConfirmed=$null}
    $receipt.processes+=@($entry);Save-Receipt;$clock=[Diagnostics.Stopwatch]::StartNew()
    try {
        $process=Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null=$process.Handle;$entry.pid=$process.Id;Save-Receipt
        if(-not $process.WaitForExit($Seconds*1000)) {
            $entry.timedOut=$true
            $kill=Start-Process -FilePath "$env:SystemRoot\System32\taskkill.exe" -ArgumentList "/PID $($process.Id) /T /F" -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run ($Name+'.kill.log')) -RedirectStandardError (Join-Path $run ($Name+'.kill-error.log'))
            if(-not $kill.WaitForExit(5000)){throw 'Owned process-tree termination deadline exceeded'}
            $entry.killActualExit=$kill.ExitCode;$entry.killConfirmed=$process.WaitForExit(5000)
            if(-not $entry.killConfirmed){throw 'Owned process termination not confirmed'}
            $entry.reportedExit=124
        } else {$entry.reportedExit=$process.ExitCode}
        $entry.actualExit=$process.ExitCode
    } catch {$entry.startOrWaitError=$_.Exception.Message;throw}
    finally {$entry.elapsedSeconds=$clock.Elapsed.TotalSeconds;Save-Receipt}
    return $entry
}
try {
    $inputs=@('tests/packs/run-codex-transactions.ps1','tests/packs/pack_codex_transaction_tests.cpp')+@(Get-ChildItem -LiteralPath (Join-Path $repo 'windows/src/effects/packs') -File | Where-Object {$_.Extension -in '.cpp','.h'} | ForEach-Object {'windows/src/effects/packs/'+$_.Name})
    foreach($relative in $inputs) {
        $path=Join-Path $repo $relative;$pre[$relative]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $copy=Join-Path $run ('sources\'+$relative);New-Item -ItemType Directory -Path (Split-Path -Parent $copy) -Force|Out-Null;Copy-Item -LiteralPath $path -Destination $copy
    }
    $receipt.sourceHashes=$pre;Save-Receipt
    $installed='C:\vcpkg\installed\x64-windows';$env:PATH=(Join-Path $installed 'bin')+';'+$env:PATH
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if(-not $vs){throw 'Installed MSVC toolchain unavailable'}
    $vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $deps=@($vcvars,(Join-Path $installed 'include/wx/version.h'),(Join-Path $installed 'lib/wxbase33u.lib'))
    foreach($dep in $deps){$receipt.dependencies[$dep]=(Get-FileHash -LiteralPath $dep -Algorithm SHA256).Hash}
    $snapshot=Join-Path $run 'sources'
    $sources=@('tests/packs/pack_codex_transaction_tests.cpp','windows/src/effects/packs/pack_manager.cpp','windows/src/effects/packs/pack_manifest.cpp','windows/src/effects/packs/pack_validator.cpp','windows/src/effects/packs/pack_preview_decoder.cpp','windows/src/effects/packs/sha256.cpp')
    $quoted=($sources|ForEach-Object {'"'+(Join-Path $snapshot $_)+'"'})-join ' '
    $exe=Join-Path $run 'pack_codex_transactions.exe';$compileFile=Join-Path $run 'compile.cmd'
    $compile='@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+'if errorlevel 1 exit /b %errorlevel%'+"`r`n"+
      'cl.exe /nologo /O2 /EHsc /std:c++17 /W4 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"'+(Join-Path $installed 'include')+'" /I"'+$snapshot+'" '+$quoted+
      ' /link /LIBPATH:"'+(Join-Path $installed 'lib')+'" ole32.lib windowscodecs.lib bcrypt.lib /OUT:"'+$exe+'"'+"`r`nexit /b %errorlevel%`r`n"
    [IO.File]::WriteAllText($compileFile,$compile,[Text.Encoding]::ASCII);$receipt.compileCommand=$compile;Save-Receipt
    $child=Invoke-Owned 'compile' 'cmd.exe' ('/d /c ""'+$compileFile+'""') $TimeoutSeconds
    if($child.reportedExit -ne 0){Get-Content -LiteralPath $child.stdout;Get-Content -LiteralPath $child.stderr;throw ('Compiler exit '+$child.reportedExit)}
    $receipt.binarySha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    $child=Invoke-Owned 'tests' $exe ('"'+(Join-Path $run 'fixtures')+'"') $TimeoutSeconds
    Get-Content -LiteralPath $child.stdout
    if($child.reportedExit -ne 0){Get-Content -LiteralPath $child.stderr;throw ('Tests exit '+$child.reportedExit)}
    $child=Invoke-Owned 'exit-proof' $exe '--exit-proof' 5
    if($child.actualExit -ne 42 -or $child.timedOut){throw 'Actual exit42 proof failed'}
    $child=Invoke-Owned 'watchdog-proof' $exe '--hang-proof' 2
    if(-not $child.timedOut -or $child.reportedExit -ne 124 -or -not $child.killConfirmed){throw 'Actual owned child timeout proof failed'}
    $overall=0
} catch {$receipt.error=$_.Exception.Message;Write-Host ('FAIL: '+$receipt.error)}
finally {
    $post=[ordered]@{};$unchanged=$true
    foreach($relative in $inputs){$path=Join-Path $repo $relative;if(Test-Path -LiteralPath $path){$post[$relative]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}else{$post[$relative]='MISSING'};if($pre[$relative] -ne $post[$relative]){$unchanged=$false}}
    $receipt.sourceHashesAfter=$post;$receipt.sourcesUnchanged=$unchanged;if(-not $unchanged){$overall=1}
    $evidence=[ordered]@{}
    Get-ChildItem -LiteralPath $run -File|Where-Object {$_.Name -ne 'receipt.json'}|ForEach-Object {$evidence[$_.FullName]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    $receipt.evidenceHashes=$evidence;$receipt.completed=$true;$receipt.successful=($overall -eq 0);$receipt.actualRunnerExit=$overall;$receipt.completedUtc=[DateTime]::UtcNow.ToString('o');Save-Receipt
    Write-Host ('Receipt: '+$receiptPath)
}
exit $overall

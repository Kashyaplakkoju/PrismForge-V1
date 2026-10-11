param([ValidateRange(10,180)][int]$TimeoutSeconds=60)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runId=(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString().Substring(0,8)
$run=Join-Path $repo ('build\pack-codex-preview-'+$runId)
New-Item -ItemType Directory -Path $run | Out-Null
$receiptPath=Join-Path $run 'receipt.json'
$receipt=[ordered]@{schemaVersion=1;runId=$runId;startedUtc=[DateTime]::UtcNow.ToString('o');completed=$false;successful=$false;processes=@();sourcesBefore=[ordered]@{};sourcesAfter=[ordered]@{};dependencyHashesBefore=[ordered]@{};dependencyHashesAfter=[ordered]@{}}
function Save-Receipt { $receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $receiptPath -Encoding UTF8 }
Save-Receipt
function Invoke-Bounded([string]$Name,[string]$Exe,[string]$Arguments,[int]$Deadline) {
    $entry=[ordered]@{name=$Name;executable=$Exe;arguments=$Arguments;deadlineSeconds=$Deadline;startedUtc=[DateTime]::UtcNow.ToString('o');actualExit=$null;reportedExit=125;timedOut=$false;terminationConfirmed=$false}
    $entry.stdout=Join-Path $run ($Name+'.stdout.log');$entry.stderr=Join-Path $run ($Name+'.stderr.log')
    $clock=[Diagnostics.Stopwatch]::StartNew()
    try {
        $proc=Start-Process -FilePath $Exe -ArgumentList $Arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput $entry.stdout -RedirectStandardError $entry.stderr
        $null=$proc.Handle;$entry.pid=$proc.Id
        if(-not $proc.WaitForExit($Deadline*1000)) {
            $entry.timedOut=$true
            $killer=Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\taskkill.exe') -ArgumentList ('/PID '+$proc.Id+' /T /F') -WindowStyle Hidden -PassThru
            if(-not $killer.WaitForExit(5000)) { $killer.Kill();$null=$killer.WaitForExit(2000) }
            $entry.killExit=if($killer.HasExited){$killer.ExitCode}else{$null}
            $entry.terminationConfirmed=$proc.WaitForExit(5000)
            $entry.reportedExit=124
        } else {$entry.terminationConfirmed=$true;$entry.reportedExit=$proc.ExitCode}
        if($proc.HasExited){$entry.actualExit=$proc.ExitCode}
    } catch {$entry.startupError=$_.Exception.Message}
    $clock.Stop();$entry.elapsedSeconds=$clock.Elapsed.TotalSeconds
    $receipt.processes+=,$entry;Save-Receipt
    return $entry
}
$overall=1
try {
    $relative=@('tests/packs/run-codex-preview.ps1','tests/packs/pack_codex_preview_tests.cpp','windows/src/effects/packs/pack_preview_decoder.h','windows/src/effects/packs/pack_preview_decoder.cpp','windows/src/effects/packs/pack_validator.h','windows/src/effects/packs/pack_validator.cpp','windows/src/effects/packs/pack_manifest.h','windows/src/effects/packs/pack_manifest.cpp','windows/src/effects/packs/sha256.h','windows/src/effects/packs/sha256.cpp')
    foreach($directory in @('tests/packs/codex_preview_fixtures','windows/src/effects/packs/samples')) {
        foreach($file in Get-ChildItem -LiteralPath (Join-Path $repo $directory) -Recurse -File) {$relative+= $file.FullName.Substring($repo.Length+1).Replace('\','/')}
    }
    $snapshot=Join-Path $run 'sources'
    foreach($rel in $relative){$path=Join-Path $repo $rel;$receipt.sourcesBefore[$rel]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;$dest=Join-Path $snapshot $rel;New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force|Out-Null;Copy-Item -LiteralPath $path -Destination $dest}
    Save-Receipt
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if(-not $vs){throw 'Installed C++ toolchain unavailable'}
    $vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $receipt.toolchainSetup=$vcvars;$receipt.toolchainSetupSha256=(Get-FileHash -LiteralPath $vcvars -Algorithm SHA256).Hash
    $sources=@('tests/packs/pack_codex_preview_tests.cpp','windows/src/effects/packs/pack_preview_decoder.cpp','windows/src/effects/packs/pack_validator.cpp','windows/src/effects/packs/pack_manifest.cpp','windows/src/effects/packs/sha256.cpp')
    $flags='/nologo /O2 /EHsc /std:c++17 /W4 /D_CRT_SECURE_NO_WARNINGS /utf-8'
    $preamble='@echo off'+"`r`n"+'call "'+$vcvars+'" >nul 2>&1'+"`r`n"+'if errorlevel 1 exit /b %errorlevel%'+"`r`n"+'set VSLANG=1033'+"`r`n"
    # Preprocessing discovers the actual SDK/STL inputs. Hash them before the
    # real compile, then prove unchanged afterwards; compilation uses preserved source.
    $preprocess=$preamble+'where cl.exe > compiler-path.txt'+"`r`n"
    foreach($rel in $sources){$preprocess+='cl.exe '+$flags+' /Zs /showIncludes "'+(Join-Path $snapshot $rel)+'"'+"`r`n"+'if errorlevel 1 exit /b %errorlevel%'+"`r`n"}
    $preprocess+='exit /b 0'+"`r`n";$preCmd=Join-Path $run 'preprocess.cmd';[IO.File]::WriteAllText($preCmd,$preprocess,[Text.Encoding]::ASCII)
    $child=Invoke-Bounded 'preprocess' 'cmd.exe' ('/d /c ""'+$preCmd+'""') $TimeoutSeconds
    if($child.reportedExit-ne 0){Get-Content -LiteralPath $child.stdout;throw ('Preprocessor exit '+$child.reportedExit)}
    $dependencies=@((Get-Content -LiteralPath (Join-Path $run 'compiler-path.txt')|Select-Object -First 1),$vcvars)
    foreach($line in Get-Content -LiteralPath $child.stdout){if($line -match 'including file:\s*(.+)$'){$dependencies+=$Matches[1].Trim()}}
    $sdk=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
    $sdkVersion=(Get-ChildItem -LiteralPath (Join-Path $sdk 'Lib') -Directory|Where-Object {$_.Name -match '^10\.'}|Sort-Object Name -Descending|Select-Object -First 1).Name
    foreach($lib in @('windowscodecs.lib','ole32.lib','kernel32.lib','uuid.lib')){$dependencies+=Join-Path $sdk ('Lib\'+$sdkVersion+'\um\x64\'+$lib)}
    $dependencies=$dependencies|Sort-Object -Unique
    foreach($path in $dependencies){if(Test-Path -LiteralPath $path -PathType Leaf){$receipt.dependencyHashesBefore[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}else{throw ('Missing recorded dependency '+$path)}}
    Save-Receipt
    $exe=Join-Path $run 'pack_codex_preview_tests.exe';$compile=$preamble+'cl.exe '+$flags+' '+(($sources|ForEach-Object{'"'+(Join-Path $snapshot $_)+'"'})-join ' ')+' /link windowscodecs.lib ole32.lib /OUT:"'+$exe+'"'+"`r`nexit /b %errorlevel%`r`n"
    $compileCmd=Join-Path $run 'compile.cmd';[IO.File]::WriteAllText($compileCmd,$compile,[Text.Encoding]::ASCII);$receipt.compileCommand=$compile
    $child=Invoke-Bounded 'compile' 'cmd.exe' ('/d /c ""'+$compileCmd+'""') $TimeoutSeconds
    if($child.reportedExit-ne 0){Get-Content -LiteralPath $child.stdout;Get-Content -LiteralPath $child.stderr;throw ('Compiler exit '+$child.reportedExit)}
    $receipt.binarySha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    $args='"'+(Join-Path $snapshot 'tests/packs/codex_preview_fixtures')+'" "'+(Join-Path $snapshot 'windows/src/effects/packs/samples')+'"'
    $child=Invoke-Bounded 'tests' $exe $args $TimeoutSeconds;Get-Content -LiteralPath $child.stdout
    if($child.reportedExit-ne 0){Get-Content -LiteralPath $child.stderr;throw ('Tests exit '+$child.reportedExit)}
    $child=Invoke-Bounded 'nonzero-proof' $exe '--exit-proof' 5
    if($child.actualExit-ne 42 -or $child.timedOut){throw 'Expected actual child exit42'}
    $child=Invoke-Bounded 'watchdog-proof' $exe '--hang-proof' 2
    if(-not $child.timedOut -or $child.reportedExit-ne 124 -or -not $child.terminationConfirmed){throw 'Expected bounded scoped termination124'}
    $overall=0
} catch {$receipt.error=$_.Exception.Message;Write-Host ('FAIL '+$receipt.error)} finally {
    $unchanged=$true
    foreach($rel in @($receipt.sourcesBefore.Keys)){$path=Join-Path $repo $rel;if(Test-Path -LiteralPath $path){$hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;$receipt.sourcesAfter[$rel]=$hash;if($hash-ne $receipt.sourcesBefore[$rel]){$unchanged=$false}}else{$unchanged=$false}}
    foreach($path in @($receipt.dependencyHashesBefore.Keys)){if(Test-Path -LiteralPath $path){$hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;$receipt.dependencyHashesAfter[$path]=$hash;if($hash-ne $receipt.dependencyHashesBefore[$path]){$unchanged=$false}}else{$unchanged=$false}}
    $receipt.sourcesAndDependenciesUnchanged=$unchanged;if(-not $unchanged){$overall=1}
    $receipt.evidenceHashes=[ordered]@{}
    foreach($file in Get-ChildItem -LiteralPath $run -File){if($file.FullName-ne $receiptPath){$receipt.evidenceHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}}
    $receipt.completed=$true;$receipt.successful=$overall-eq 0;$receipt.actualRunnerExit=$overall;$receipt.completedUtc=[DateTime]::UtcNow.ToString('o');Save-Receipt
    Write-Host ('Receipt: '+$receiptPath)
}
exit $overall

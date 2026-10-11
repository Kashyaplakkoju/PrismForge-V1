param([string]$VcpkgRoot='C:\vcpkg',[string]$BaselineCommit='86726cc493edc1560b86102a0638f309e7e72f5f',[string]$ReferenceRoot='G:\Projects\ThirdParty\VCamdroid')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build=Join-Path $repo 'build\mf-writer-benchmark'
$baselineDir=Join-Path $build 'baseline';$referenceDir=Join-Path $build 'reference'
New-Item -ItemType Directory -Path $build,$baselineDir,$referenceDir -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$inc=Join-Path $VcpkgRoot 'installed\x64-windows\include';$lib=Join-Path $VcpkgRoot 'installed\x64-windows\lib'
$testSource=Join-Path $repo 'tests\mftransport\writer_benchmark.cpp'
if (!(Test-Path -LiteralPath $vcvars) -or !(Test-Path -LiteralPath $inc)) { throw 'Existing MSVC and FFmpeg/vcpkg are required.' }
$utf8=New-Object System.Text.UTF8Encoding($false)
foreach($name in @('MFSharedMemWriter.cpp','MFSharedMemWriter.h')) {
    $baselineText=& git -C $repo show "${BaselineCommit}:windows/src/mfsource/$name"
    if ($LASTEXITCODE -ne 0) { throw 'Unable to read exact pre-change writer from Git.' }
    [IO.File]::WriteAllLines((Join-Path $baselineDir $name),[string[]]$baselineText,$utf8)
}
# Read-only reference snapshot; replace only IPC names with private Local objects.
foreach($name in @('MFSharedMemWriter.cpp','MFSharedMemWriter.h','FrameServer.h')) {
    $source=Join-Path $ReferenceRoot ('windows\src\mfsource\'+$name)
    $text=[IO.File]::ReadAllText($source,$utf8)
    if($name -eq 'MFSharedMemWriter.cpp') { $text=$text.Replace('#include "MFSharedMemWriter.h"',"#include `"MFSharedMemWriter.h`"`r`n#include `"benchmark_names.h`"") }
    $text=$text.Replace('L"Global\\VCamdroid_SharedMem"','BenchmarkMapName()').Replace('L"Local\\VCamdroid_SharedMem"','BenchmarkMapName()')
    $text=$text.Replace('L"Global\\VCamdroid_Mutex"','BenchmarkMutexName()').Replace('L"Local\\VCamdroid_Mutex"','BenchmarkMutexName()')
    [IO.File]::WriteAllText((Join-Path $referenceDir $name),$text,$utf8)
}
$names=@"
#pragma once
#include <windows.h>
#include <string>
inline const WCHAR* BenchmarkMapName() {
    static std::wstring value=L"Local\\PrismForge_WriterBench_"+std::to_wstring(GetCurrentProcessId())+L"_reference_map";
    return value.c_str();
}
inline const WCHAR* BenchmarkMutexName() {
    static std::wstring value=L"Local\\PrismForge_WriterBench_"+std::to_wstring(GetCurrentProcessId())+L"_reference_mutex";
    return value.c_str();
}
"@
[IO.File]::WriteAllText((Join-Path $referenceDir 'benchmark_names.h'),$names,$utf8)
$variants=@(
    @{Name='baseline';Folder=$baselineDir;Source=(Join-Path $baselineDir 'MFSharedMemWriter.cpp');Define=''},
    @{Name='current';Folder=(Join-Path $repo 'windows\src\mfsource');Source=(Join-Path $repo 'windows\src\mfsource\MFSharedMemWriter.cpp');Define=''},
    @{Name='reference';Folder=$referenceDir;Source=(Join-Path $referenceDir 'MFSharedMemWriter.cpp');Define='/DMF_REFERENCE_WRITER'}
)
foreach($variant in $variants) {
    $objectDir=Join-Path $build ($variant.Name+'-obj');New-Item -ItemType Directory -Path $objectDir -Force | Out-Null
    $exe=Join-Path $build ($variant.Name+'.exe')
    $compile='call "'+$vcvars+'" >nul 2>&1 && cl.exe /nologo /MP /O2 /EHsc /std:c++17 /W3 /DNOMINMAX '+$variant.Define+' /I"'+$variant.Folder+'" /I"'+$inc+'" /I"'+$repo+'\windows\src" /I"'+$repo+'\windows\src\mfsource" "'+$testSource+'" "'+$variant.Source+'" /Fo:"'+$objectDir+'\\" /link /LIBPATH:"'+$lib+'" avutil.lib avcodec.lib avformat.lib swscale.lib advapi32.lib /OUT:"'+$exe+'"'
    cmd.exe /c $compile
    if ($LASTEXITCODE -ne 0) { throw "Benchmark compilation failed ($($variant.Name)): $LASTEXITCODE" }
}
$oldPath=$env:PATH
try {
    $env:PATH=(Join-Path $VcpkgRoot 'installed\x64-windows\bin')+';'+$env:PATH
    $run=0
    foreach($name in @('baseline','current','reference','reference','current','baseline')) {
        ++$run;$info=New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName=Join-Path $build ($name+'.exe');$info.WorkingDirectory=$build
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $process=New-Object System.Diagnostics.Process;$process.StartInfo=$info
        try {
            if(!$process.Start()){throw 'Unable to start benchmark.'}
            $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
            if(!$process.WaitForExit(60000)){$process.Kill();$process.WaitForExit();throw 'Benchmark exceeded 60s watchdog.'}
            $exitCode=$process.ExitCode;$outputText=$stdout.GetAwaiter().GetResult();$errorText=$stderr.GetAwaiter().GetResult()
            [IO.File]::WriteAllText((Join-Path $build ("$run-$name.log")),$outputText,$utf8)
            [IO.File]::WriteAllText((Join-Path $build ("$run-$name.stderr.log")),$errorText,$utf8)
            Write-Output "RUN $run $name EXIT $exitCode";Write-Output $outputText;if($errorText){Write-Output $errorText}
            if($exitCode -ne 0){throw "Benchmark failed ($name): $exitCode"}
        }finally{$process.Dispose()}
    }
    $receipt=[ordered]@{CreatedUtc=[DateTime]::UtcNow.ToString('o');BaselineCommit=$BaselineCommit;ReferenceRoot=$ReferenceRoot;Scope='1080p software MF writer only; 24 warmup +360 changing-pixel samples per format, private Local IPC, forward/reverse runs; excludes capture/decode/preview/DirectShow/consumer';Sources=@()}
    foreach($path in @((Join-Path $baselineDir 'MFSharedMemWriter.cpp'),(Join-Path $repo 'windows\src\mfsource\MFSharedMemWriter.cpp'),(Join-Path $ReferenceRoot 'windows\src\mfsource\MFSharedMemWriter.cpp'),$testSource)) {$receipt.Sources+=@{Path=$path;SHA256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}}
    [IO.File]::WriteAllText((Join-Path $build 'receipt.json'),($receipt|ConvertTo-Json -Depth 5),$utf8)
}finally{$env:PATH=$oldPath}

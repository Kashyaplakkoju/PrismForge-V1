param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
# The provider compiles against a pinned local SDK; no global runtime installation.
$ortHeader = Join-Path $repoRoot 'build/provider-deps/ort-sdk/include/onnxruntime_c_api.h'
if (-not (Test-Path -LiteralPath $ortHeader)) {
    & (Join-Path $repoRoot 'tools/provider-deps/bootstrap_ort.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Pinned inference SDK setup failed.' }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild with the C++ toolchain is required.' }
$wxRuntime = Join-Path $repoRoot 'build/deps/wx-build/lib/vc_x64_dll/wxmsw333u_core_vc_x64_studio.dll'
if (-not (Test-Path -LiteralPath $wxRuntime)) {
    & python (Join-Path $repoRoot 'tools/setup_windows_ui.py')
    if ($LASTEXITCODE -ne 0) { throw 'Studio UI dependency build failed.' }
}
if ($Configuration -eq 'Debug') {
    & cmake --build (Join-Path $repoRoot 'build/deps/wx-build') --config Debug --target wxcore --parallel 8
    if ($LASTEXITCODE -ne 0) { throw 'Debug UI dependency build failed.' }
}
& (Join-Path $repoRoot 'windows/build_vcam_components.bat')
if ($LASTEXITCODE -ne 0) { throw 'Modern virtual-camera component build failed.' }
$solutions = @(
    'windows/3rdparty/softcam/softcam.sln',
    'windows/3rdparty/softcam/examples/softcam_installer/softcam_installer.sln',
    'windows/PrismForgeStudio.sln'
)
foreach ($solution in $solutions) {
    & $msbuild (Join-Path $repoRoot $solution) "/p:Configuration=$Configuration" /p:Platform=x64 /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $solution" }
}

# New StudioStack CPU fallback uses MSVC OpenMP; stage its matching redistributable.
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$openMpDir = Get-ChildItem -Path (Join-Path $vsInstall 'VC/Redist/MSVC/*/x64/Microsoft.VC*.OpenMP') -Directory | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $openMpDir) { throw 'Matching Microsoft OpenMP app-local runtime not found.' }
Copy-Item -LiteralPath (Join-Path $openMpDir.FullName 'vcomp140.dll') -Destination (Join-Path $repoRoot 'dist')

& (Join-Path $repoRoot 'tools/provider-deps/acquire-person-model.ps1') -StageDirectory (Join-Path $repoRoot 'dist')
if ($LASTEXITCODE -ne 0) { throw 'Verified person segmentation dependencies could not be staged.' }

# Native face analysis assets are app-local and verified before staging.
& python (Join-Path $repoRoot 'tools/face-deps/acquire_face_assets.py')
if ($LASTEXITCODE -ne 0) { throw 'Pinned face assets could not be acquired.' }
& python (Join-Path $repoRoot 'tools/face-deps/stage_face_assets.py') (Join-Path $repoRoot 'dist')
if ($LASTEXITCODE -ne 0) { throw 'Verified face assets could not be staged.' }

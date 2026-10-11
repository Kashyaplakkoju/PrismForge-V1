param([string]$OutputPath, [string]$DistPath)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $OutputPath) { $OutputPath = Join-Path $repoRoot 'PrismForgeStudio' }
$outputRoot = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $outputRoot) { throw "Output already exists; choose a fresh directory: $outputRoot" }
$dist = if ($DistPath) { [IO.Path]::GetFullPath($DistPath) } else { Join-Path $repoRoot 'dist' }
$required = @('PrismForgeStudio.exe', 'prismforge_cam.dll', 'prismforge_cam_installer.exe', 'prismforge_source.dll', 'prismforge_vcam_installer.exe', 'apk/prismforge-companion-debug.apk', 'adb/adb.exe', 'vcomp140.dll', 'onnxruntime.dll', 'models/pphumanseg/model.onnx', 'models/pphumanseg/manifest.json', 'models/pphumanseg/LICENSE', 'models/pphumanseg/NOTICE', 'licenses/onnxruntime/LICENSE', 'licenses/onnxruntime/ThirdPartyNotices.txt', 'libmediapipe.dll', 'models/face-landmarker/face_landmarker.task', 'models/face-landmarker/canonical_face_model.obj', 'models/face-landmarker/LICENSE', 'models/face-landmarker/NOTICE.txt', 'licenses/mediapipe/LICENSE')
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $dist $file))) { throw "Build artifact missing: $file" }
}
New-Item -ItemType Directory -Path $outputRoot | Out-Null
foreach ($file in @('PrismForgeStudio.exe', 'prismforge_cam.dll', 'prismforge_cam_installer.exe', 'prismforge_source.dll', 'prismforge_vcam_installer.exe')) {
    Copy-Item -LiteralPath (Join-Path $dist $file) -Destination $outputRoot
}
# Legacy camera binaries and executables must never enter the new package.
Get-ChildItem -LiteralPath $dist -Filter '*.dll' | Where-Object { $_.Name -notmatch '^(softcam|softcamd|prismforge_cam_d|vcamdroid_source|wx.*331.*)\.dll$' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $outputRoot }
foreach ($directory in @('adb', 'res', 'apk', 'models')) {
    if ($directory -eq 'apk') {
        New-Item -ItemType Directory -Path (Join-Path $outputRoot 'apk') | Out-Null
        Copy-Item -LiteralPath (Join-Path $dist 'apk/prismforge-companion-debug.apk') -Destination (Join-Path $outputRoot 'apk')
    } elseif ($directory -eq 'res') {
        $resourceDir = Join-Path $outputRoot 'res'
        New-Item -ItemType Directory -Path $resourceDir | Out-Null
        Get-ChildItem -LiteralPath (Join-Path $dist 'res') -File | Where-Object { $_.Extension -eq '.png' -or ($_.Extension -eq '.ico' -and $_.Name -ne 'vcamdroid.ico') } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $resourceDir }
        $capResourceDir=Join-Path $resourceDir 'accessories'
        New-Item -ItemType Directory -Path $capResourceDir | Out-Null
        foreach($capAsset in @('pokemon_masters_cap.pfrgba','NOTICE.txt')) {
            Copy-Item -LiteralPath (Join-Path $dist ('res/accessories/'+$capAsset)) -Destination $capResourceDir
        }
        $filterResourceDir=Join-Path $resourceDir 'face-filters'
        New-Item -ItemType Directory -Path $filterResourceDir | Out-Null
        foreach($filterAsset in @('aurora-fox-mask.pfmodel','nocturne-owl-mask.pfmodel','atlas-explorer-helmet.pfmodel','NOTICE.txt','manifest.json')) {
            Copy-Item -LiteralPath (Join-Path $dist ('res/face-filters/'+$filterAsset)) -Destination $filterResourceDir
        }
    } else {
        Copy-Item -LiteralPath (Join-Path $dist $directory) -Destination $outputRoot -Recurse
    }
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'assets/starter-packs') -Destination (Join-Path $outputRoot 'packs') -Recurse
$scriptsDir = Join-Path $outputRoot 'scripts'
New-Item -ItemType Directory -Path $scriptsDir | Out-Null
foreach ($script in @('install.bat', 'uninstall.bat', 'install_apk.bat')) { Copy-Item -LiteralPath (Join-Path $repoRoot $script) -Destination $scriptsDir }
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $outputRoot
$noticesDir = Join-Path $outputRoot 'licenses'
New-Item -ItemType Directory -Path $noticesDir | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'windows/3rdparty/softcam/LICENSE') -Destination (Join-Path $noticesDir 'Softcam-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repoRoot 'build/deps/wx-src/docs/licence.txt') -Destination (Join-Path $noticesDir 'wxWidgets-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repoRoot 'QR-Code-generator/Readme.markdown') -Destination (Join-Path $noticesDir 'QR-Code-generator-Readme.txt')
Set-Content -LiteralPath (Join-Path $outputRoot 'DEVELOPMENT-BUILD.txt') -Value 'Local development build. APK uses debug signing. See BUILD-INFO.json for source provenance and the repository verification receipts for tested checkpoints. Clean-machine installation and dependency redistribution review are pending. Not a public release.'
# Include the matching app-local Microsoft runtime; packaging never installs it.
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crtDir = Get-ChildItem -Path (Join-Path $vsInstall 'VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT') -Directory | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crtDir) { throw 'Microsoft C++ app-local runtime not found.' }
Get-ChildItem -LiteralPath $crtDir.FullName -Filter '*.dll' | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $outputRoot }
Copy-Item -LiteralPath (Join-Path $dist 'licenses/onnxruntime') -Destination $noticesDir -Recurse
Copy-Item -LiteralPath (Join-Path $dist 'licenses/mediapipe') -Destination $noticesDir -Recurse
$cgltfNoticesDir = Join-Path $noticesDir 'cgltf'
New-Item -ItemType Directory -Path $cgltfNoticesDir | Out-Null
foreach ($notice in @('LICENSE','NOTICE.txt')) { Copy-Item -LiteralPath (Join-Path $repoRoot ('windows/3rdparty/cgltf/'+$notice)) -Destination $cgltfNoticesDir }
$vsNotices = Join-Path $vsInstall 'Licenses/1033'
foreach ($notice in @('Redist.txt', 'ThirdPartyNotices.txt')) {
    Copy-Item -LiteralPath (Join-Path $vsNotices $notice) -Destination (Join-Path $noticesDir ('Microsoft-' + $notice))
}
& python (Join-Path $repoRoot 'tools/verify_development_package.py') $outputRoot
if ($LASTEXITCODE -ne 0) { throw 'Package dependency verification failed.' }
& python (Join-Path $repoRoot 'tools/verify_native_face_filters.py') $outputRoot
if ($LASTEXITCODE -ne 0) { throw 'Native 3D filter asset verification failed.' }
& python (Join-Path $repoRoot 'tools/verify_segmentation_assets.py') $outputRoot
if ($LASTEXITCODE -ne 0) { throw 'Package dependency verification failed.' }
& python (Join-Path $repoRoot 'tools/face-deps/stage_face_assets.py') $outputRoot --verify-only
if ($LASTEXITCODE -ne 0) { throw 'Face asset verification failed.' }
$integrationCommit = (& git -C $repoRoot rev-parse HEAD)
if ($LASTEXITCODE -ne 0 -or $integrationCommit -notmatch '^[0-9a-fA-F]{40}$') { throw 'Unable to identify the package source commit.' }
& git -C $repoRoot diff --quiet HEAD --
if ($LASTEXITCODE -ne 0) { throw 'Commit the reviewed tracked source changes before packaging.' }
$buildInfo = [PSCustomObject]@{
    Product = 'PrismForge Studio'; UpstreamStudioCommit = 'f7b065f'; IntegrationCommit = $integrationCommit
    AndroidSourceCommit = '60c2bda1dd335adaade73a82abf0eeafea3de003'; PersonModel = 'OpenCV Zoo PP-HumanSeg 2023mar'; FaceModel = 'MediaPipe FaceLandmarker float16 v1'; FaceRuntime = 'MediaPipe0.10.32 Windows x64 native CPU'
    Kind = 'Local development'; AndroidSigning = 'Debug'; CreatedUtc = [DateTime]::UtcNow.ToString('o')
}
$buildInfo | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'BUILD-INFO.json')
$manifest = Get-ChildItem -LiteralPath $outputRoot -File -Recurse | ForEach-Object {
    [PSCustomObject]@{ Path = $_.FullName.Substring($outputRoot.Length + 1); SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $outputRoot 'manifest.json')
Write-Output "Development package: $outputRoot"

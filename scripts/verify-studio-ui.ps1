[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ExePath, [switch]$SkipScrolling, [ValidateRange(1,120)][int]$TimeoutSeconds=60)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$workingDir = Split-Path $exe -Parent
$logPath = Join-Path $workingDir 'prismforge.log'
# The app logger uses its executable directory. Avoid truncating another instance's log.
$existing = @(Get-Process PrismForgeStudio -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })
if ($existing.Count) { throw 'This executable is already running. Use a separate staged build for UI verification.' }
$oldPreview = $env:PRISMFORGE_UI_PREVIEW
$oldVerify = $env:PRISMFORGE_UI_VERIFY_LOOKS
$oldScroll = $env:PRISMFORGE_UI_VERIFY_SCROLLING
$oldSources = $env:PRISMFORGE_UI_VERIFY_SOURCES
$oldControls = $env:PRISMFORGE_UI_VERIFY_CONTROLS
$oldImageFixtures = $env:PRISMFORGE_UI_VERIFY_IMAGE_FIXTURES
$process = $null
try {
    $env:PRISMFORGE_UI_PREVIEW = '1'
    $env:PRISMFORGE_UI_VERIFY_LOOKS = '1'
    $env:PRISMFORGE_UI_VERIFY_SCROLLING = if ($SkipScrolling) { $null } else { '1' }
    $env:PRISMFORGE_UI_VERIFY_SOURCES = '1'
    $env:PRISMFORGE_UI_VERIFY_CONTROLS = '1'
    $env:PRISMFORGE_UI_VERIFY_IMAGE_FIXTURES = (Resolve-Path (Join-Path $PSScriptRoot '..\tests\integration-engine\image-fixtures')).Path
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $exe
    $startInfo.WorkingDirectory = $workingDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    # This is a native paint test: the preview must be visible to receive real paint damage.
    $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Normal
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (!$process.Start()) { throw 'Unable to start isolated UI verification.' }
    $verificationClock=[Diagnostics.Stopwatch]::StartNew()
    if (!$process.WaitForExit($TimeoutSeconds*1000)) { throw "Isolated UI verification timed out after $TimeoutSeconds seconds." }
    $verificationClock.Stop()
    if ($process.ExitCode -ne 0) { throw "Isolated UI verification exited with $($process.ExitCode)." }
    $log = Get-Content -LiteralPath $logPath -Raw
    if ($log -notmatch '\[UI-VERIFY\] Custom LUT controls/pixels PASS') { throw 'Custom LUT controls/pixels did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Desktop look event routing PASS') { throw 'UI verification success marker is missing.' }
    if (!$SkipScrolling -and $log -notmatch '\[UI-VERIFY\] Inspector scrolling PASS') { throw 'Inspector scrolling verification did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Source controls PASS') { throw 'System camera source controls did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Studio lighting/frame controls/pixels PASS') { throw 'Studio lighting/frame controls and actual pixels did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Custom two-light controls/pixels PASS') { throw 'Custom two-light event routing, pixels and retention did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Light dragging avoids inspector layout PASS') { throw 'Light dragging still rebuilds the inspector.' }
    if ($log -notmatch '\[UI-VERIFY\] Face touch-up labels/bounds/no-layout PASS') { throw 'Face touch-up labels, status bounds and no-layout controls did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Face touch-up settings/coexistence/bypass PASS') { throw 'Face touch-up settings and shared effects bypass did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Face accessories labels/bounds/events/no-layout PASS') { throw 'Face accessory control checks did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Face accessories settings/coexistence/bypass PASS') { throw 'Face accessory settings and shared bypass checks did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] 3D face filters controls PASS') { throw 'Native 3D filter control checks did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] 3D face filters settings/coexistence/bypass PASS') { throw 'Native 3D model selection, fit persistence and bypass did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Royal crown default/independent fit/reload/local restore PASS') { throw 'Royal crown default and independent saved fit did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Model performance dispatch/persistence/busy/bypass/hold/restore PASS') { throw 'Model performance dispatch, saved budgets and held-model restore did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Light control labels PASS') { throw 'Light-control labels did not match the displayed text.' }
    if ($log -notmatch '\[UI-VERIFY\] Light position control PASS') { throw 'Native light-position mouse/keyboard interaction did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Background image decode/bounds PASS') { throw 'Native background image decoding and bounds checks did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Background controls PASS') { throw 'Background controls did not pass.' }
    if ($log -notmatch '\[UI-VERIFY\] Background quality routing PASS') { throw 'Background quality selection and bypass/preset retention did not pass.' }
    if ($log -match '\[CAM\]|\[SERVER\]|\[NET\]|\[RTSP\]') { throw 'Camera or network initialization unexpectedly occurred in UI verification.' }
    if ($log -notmatch '\[UI-VERIFY\] Controls layout/paint/icons PASS') { throw 'Controls layout/paint/native icons did not pass.' }
    if ($SkipScrolling) { Write-Output 'Inspector scrolling skipped at owner request.' }
    Write-Output ("Verification elapsed: {0:N2}s; actual app exit: {1}" -f $verificationClock.Elapsed.TotalSeconds,$process.ExitCode)
    Write-Output 'PASS: Studio construction, offline look/LUT selection and expected pixels, intensity, clear, bypass/restore, preset application preview-mode file-operation guards, inspector checks when enabled; grouped source keyboard navigation and capability-aware system camera controls; studio lighting/direction/strength, custom two-light mouse/keyboard positions, independent intensity/temperature and saved settings, subject-only routing, border, source generation and shared bypass.'
} finally {
    if ($process -and !$process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (!$process.WaitForExit(3000)) { $process.Kill(); $process.WaitForExit() }
    }
    $env:PRISMFORGE_UI_PREVIEW = $oldPreview
    $env:PRISMFORGE_UI_VERIFY_LOOKS = $oldVerify
    $env:PRISMFORGE_UI_VERIFY_SCROLLING = $oldScroll
    $env:PRISMFORGE_UI_VERIFY_SOURCES = $oldSources
    $env:PRISMFORGE_UI_VERIFY_CONTROLS = $oldControls
    $env:PRISMFORGE_UI_VERIFY_IMAGE_FIXTURES = $oldImageFixtures
    if ($process) { $process.Dispose() }
}

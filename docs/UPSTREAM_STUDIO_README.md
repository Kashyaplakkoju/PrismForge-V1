# VCamdroid

Use an Android phone as a webcam on Windows, connected over USB or a local Wi-Fi network.

## Run the ready-to-use package

Send `dist/share/VCamdroid-Complete-YYYY-MM-DD.zip` to share both apps and the
clean project source. The smaller `VCamdroid-Ready-to-use-YYYY-MM-DD.zip` contains
the apps only. Extract the **whole ZIP**, open `START-HERE.txt`, and launch
`Windows/Start VCamdroid.cmd`. Copy `Android/VCamdroid.apk` to your phone to install.

For meeting-app camera output, right-click `Windows/Setup Camera.cmd` and choose
**Run as administrator** once. Its runtime installer, camera components, ADB,
assets, and runtime DLLs are included; no separate development tools or DLL
downloads are required to run. Windows 11 x64 and Android 9+ are required.
Some phones need a manufacturer USB driver; Wi-Fi avoids that requirement.
Both apps include the camera-tilt fix and System, Dark, Light and Glass themes.
See [distribution details](docs/DISTRIBUTION.md). The remaining build instructions
are for developers working from the source package.

The **Studio** redesign lives on `UI-Improve`. Both apps support **System, Dark,
Light, and Glass** appearance. Android also has a separate **Dim screen** mode during
streaming; it changes brightness without changing the chosen theme.

## What's new

- Resizable desktop window with a 640 x 480 logical minimum, aspect-fit preview,
  a navigation rail, floating camera dock, collapsible settings cards, full screen, and remembered window size.
- Search actions with **Ctrl+K**, labeled controls, tooltips, and useful empty,
  connecting, live, and interrupted states.
- Meeting, Low bandwidth, and High quality presets; save, import, export, and
  remove custom quality profiles. Unsupported resolutions use the nearest camera
  capability. FPS remains a request and depends on the phone.
- Device pin markers, a reconnect action, guided QR/USB pairing, and snapshot
  feedback with unique filenames. Diagnostics export omits device identifiers.
- Android portrait/landscape layouts, system insets, larger touch targets,
  a camera control sheet, validated manual addresses, and last-computer reconnect.
- Shared theme tokens and reusable control helpers in `windows/src/gui/studio.h`
  and Android `StudioAppearance.kt`, `StudioInsets.kt`, and resource styles.

**Themes:** desktop **View -> Appearance** or the header theme toggle **(◐)**.
Phone: **Theme** on the connection screen, or **Controls -> Appearance** while
streaming. Supports **System, Dark, Light, and Glass** themes with smooth owner-drawn styling.
The app remembers your selection; System follows the operating system.

| Shortcut | Action |
| --- | --- |
| Ctrl+K | Search actions |
| Ctrl+N | Connect phone |
| Ctrl+, | Show/hide controls |
| Ctrl+R | Reconnect selected phone |
| Ctrl+Shift+S | Snapshot |
| F11 / Esc | Enter / leave full screen |
| Ctrl+H | Hide to tray |
| Ctrl+Q | Quit |

This repository and the source ZIP contain source, licenses, and build instructions.
EXE, DLL, APK, credentials, local settings, and generated caches are excluded from
the source ZIP. The ready-to-use and Complete ZIPs contain the compiled Windows
app and Android release APK as well as their runtime files. The original working
source was also archived before the redesign.

## Requirements

- **Windows 11, x64** for this version's Media Foundation virtual camera.
  Microsoft's virtual-camera API requires Windows build 22000 or newer.
  The older DirectShow output is also included; this combined version has not
  been verified on Windows 10.
- **Android 9 / API 28 or newer** (the actual `minSdk` in this source).
- **Visual Studio 2026**, Desktop development with C++, MSVC **v145**, and a
  Windows SDK that includes `mfvirtualcamera.h` (Windows 11 SDK).
- **Python 3.10+ and CMake** for the pinned local wxWidgets 3.3.3 UI build.
- **vcpkg** with x64 Windows packages: `asio` and `ffmpeg`.
  Validated here with Asio 1.32.0 and FFmpeg 8.1.2.
  There is no dependency lockfile; installing current versions is not guaranteed
  to reproduce that environment exactly.
- **Android Studio**, Android SDK platform 35, and **JDK 17 or 21** for Gradle (validated here with Android Studio JBR 21).
  The repository supplies Gradle 8.7, Android Gradle Plugin 8.6.0, and Kotlin 2.2.0.
- An internet connection to download dependencies; a physical phone for testing.

Use a short extraction path without spaces, for example `C:\src\VCamdroid`.
Some existing third-party build events do not quote paths containing spaces.

## 1. Restore external tools

Install [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started-msbuild).
From a shell where `vcpkg` is available, run:

```powershell
vcpkg install asio:x64-windows ffmpeg:x64-windows
vcpkg integrate install
python tools/setup_windows_ui.py
```

The UI setup verifies the official wxWidgets source SHA-256, applies a small
app-local appearance-switch patch, and builds Release/x64 under `build/deps`.
It does not alter an existing vcpkg installation or Windows theme. The first run
can take several minutes. Debug builds require also building that dependency
with `cmake --build build/deps/wx-build --config Debug --target wxcore --parallel 8`.

Download [Android SDK Platform-Tools for Windows](https://developer.android.com/tools/releases/platform-tools).
Copy `adb.exe`, `AdbWinApi.dll`, and `AdbWinUsbApi.dll` into `windows\adb\`.
These files are required by the existing post-build copy steps, including if you
intend to connect over Wi-Fi. They are deliberately excluded from this source ZIP.

Softcam and QR Code generator source are already bundled. Do not run a submodule
initialization command: this package contains ordinary files, not Git submodules.
The stale source repository `.gitmodules` entry was omitted for that reason.

## 2. Build the Windows app

Select **Release / x64** throughout. Keep the supplied project toolset versions.

1. Open `windows\3rdparty\softcam\softcam.sln` in Visual Studio.
   Build the **softcam** project and its dependencies. The test projects are not
   required to run VCamdroid. This generates headers, libraries and `softcam.dll`
   under `windows\3rdparty\softcam\dist\`.
2. Open `windows\3rdparty\softcam\examples\softcam_installer\softcam_installer.sln`
   and build its Release / x64 configuration.
3. Open an **x64 Native Tools Command Prompt for VS 2026**, change to the extracted
   project root, and build the Media Foundation components with the commands below.
4. Open `windows\VCamdroid.sln` and build **Release / x64**. Post-build steps
   collect the app, DLLs, resources, and ADB files in the root `dist\` folder.

For step 3, run `windows\build_vcam_components.bat`. It detects Visual Studio
with `vswhere`, compiles into `windows\x64\CameraComponents`, and copies the two
camera components into `dist`. It does not register or install anything.

Verify `dist\` contains `VCamdroid.exe`, `vcamdroid_source.dll`, `vcam_installer.exe`,
`softcam.dll`, the FFmpeg/wxWidgets runtime DLLs, `res\`, and `adb\`.

## 3. Build and install the Android app

Open `android\` in Android Studio. Use JDK 17 or 21 for Gradle, install SDK platform 35,
and let Gradle sync. Android Studio creates the excluded `local.properties` with
your own SDK path. Build from the project root in PowerShell:

```powershell
Push-Location android
.\gradlew.bat assembleDebug
Pop-Location
.\windows\adb\adb.exe devices
.\windows\adb\adb.exe install -r .\android\app\build\outputs\apk\debug\app-debug.apk
```

Before installation, enable Developer options / USB debugging on the phone and
accept its USB authorization prompt. Alternatively, copy the built APK to the
phone and open it to install. Grant camera permission when the app requests it.

The current release Gradle configuration also uses a locally generated debug key.
No signing keys are included. Installing a build signed with a different key over
an older installation may require uninstalling that older app first (losing its
local data). Use your own release signing setup for publication.

The legacy `install_apk.bat` expects a separate release-bundle layout; the direct
ADB command above matches this source package. `client.py` is a legacy development
helper, not the app entry point.

## 4. Register and run

1. Keep the extracted/built project in a stable location. Camera registration
   refers to the DLL's location.
2. Run the root `install.bat` and accept administrator elevation. This registers
   the existing Windows virtual camera and DirectShow output.
3. Launch `dist\VCamdroid.exe` normally.
4. Open VCamdroid on the phone.
5. **USB:** connect an authorized debugging device, tap **USB cable** on the
   phone, then select it in the desktop Source list. **Wi-Fi:** use the same trusted local network, open desktop
   **Connect phone**, and scan it on the phone; manual IP entry is also available.
6. Select **VCamdroid Camera** in your meeting app or Windows camera picker.
   **DirectShow Softcam** is available in applications that enumerate that output.
7. Keep the desktop app and phone stream running while using the camera.

The default Windows camera mode shows the camera while the desktop app is active.
The desktop **Device -> Windows camera visibility** menu also offers persistent visibility
and DirectShow-only modes. Register the updated DLL again after rebuilding it.
Use root `uninstall.bat` with elevation before removing registered camera files.

## Troubleshooting

- **Missing runtime DLL:** keep all generated runtime DLLs beside `VCamdroid.exe`
  and install Microsoft's [Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- **USB phone missing:** run `windows\adb\adb.exe devices`; resolve `unauthorized`
  on the phone, and check the cable and the phone manufacturer's USB driver.
- **Wi-Fi cannot connect:** check both devices are on the same network, client
  isolation is disabled, and Windows Firewall allows VCamdroid on that trusted network.
- **Windows camera missing:** verify Windows 11, rerun `install.bat`, keep the app
  open in dynamic mode, then reopen the camera picker.
- **Black external preview:** first confirm a live desktop preview, check camera
  access permissions, and allow a few seconds for the shared-memory connection.
  Close/reopen the consuming app after camera configuration changes.
- **Frame rate differs:** the phone capture rate and the rate negotiated by the
  consuming application both matter. A 60 fps setting alone does not guarantee 60 fps.
- **Build errors:** confirm Release / x64, v145, correct SDK, vcpkg integration,
  and that Softcam was built first. Dependency restore/build on a clean machine
  has not been repeated for this packaging operation.

## Source layout

- `windows/src/`: wxWidgets desktop UI, networking, decoding, and camera outputs.
- `windows/src/mfsource/`: Windows Media Foundation camera, shared-memory transport,
  and virtual-camera lifecycle management.
- `windows/src/installer/`: Windows virtual-camera installer source.
- `windows/3rdparty/softcam/`: bundled DirectShow library source and its license.
- `android/`: Android Kotlin/Java application, layouts, and Gradle wrapper.
- `QR-Code-generator/`: bundled QR generator source and copyright notices.
- `imgs/`: lightweight documentation assets (large demo GIF omitted).
- `PACKAGE-MANIFEST.json`: file hashes for every included source file.

## Validation and source packaging

Run the checks from the repository root:

```powershell
Push-Location android
.\gradlew.bat assembleDebug testDebugUnitTest lintDebug
Pop-Location
python tools/package_source.py
```

After UI setup, in an x64 Visual Studio Developer Command Prompt, run `windows\tests\run-tests.bat`. This tests profile parsing,
invalid input rejection, round trips, names, and resolution matching.

The source packager writes `dist/share/VCamdroid-Studio-source.zip`, a SHA-256
checksum, and a per-file manifest inside the archive. It excludes build outputs,
large demo media, caches, local configuration, and common credential files, and
checks archive contents and hashes. The Gradle wrapper JAR is intentionally retained.
`package.bat` now forwards arguments to `tools/package_apps.py` to create all three
shareable ZIPs; see `docs/DISTRIBUTION.md`. The stale submodule entry and local
camera investigation report/captures are omitted from the source ZIP.

Validated on the development machine: Windows Release/x64 app and camera-component
builds, native profile tests, Android debug build, JVM tests, and Android lint
(0 errors; existing/style/dependency warnings remain). Desktop preview and appearance
checks use `VCAMDROID_UI_PREVIEW=1`, which starts no network, ADB, camera registration,
or saved settings. See `docs/STUDIO-VALIDATION.md` for limits and follow-up checks.
The camera-tilt fix at `1823fb5` was verified with physical USB/Wi-Fi streams,
both cameras, and stabilization ON/OFF without rotation wedges. Exact all-stage
<0.1-degree acceptance and clean-PC camera setup are not established.

## Import the source into Git

Extract the source ZIP, run `git init`, review `git status`, then `git add .` and
commit. Softcam and the QR generator are vendored source, so no submodule setup
is needed. Keep the supplied ignore rules and third-party license notices.

## Licenses and upstream

Keep `LICENSE` and bundled third-party copyright/license notices when sharing.
Upstream: [darusc/VCamdroid](https://github.com/darusc/VCamdroid).
Softcam: [tshino/softcam](https://github.com/tshino/softcam).
Dependencies downloaded during setup retain their own licenses.

Build references:
[Windows virtual-camera API](https://learn.microsoft.com/en-us/windows/win32/api/mfvirtualcamera/nf-mfvirtualcamera-mfcreatevirtualcamera),
[Android Gradle Plugin 8.6](https://developer.android.com/build/releases/agp-8-6-0-release-notes).

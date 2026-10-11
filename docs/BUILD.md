# Build PrismForge V1

The public source is an export of V1 checkpoint `2615a0c`, with the unused
Standard-licensed visor mesh removed and public documentation added. It contains
no V2 source, private crown model or face-swap code/dependencies.

## Windows

Use Windows 11 x64, Visual Studio with C++ toolset **v145**, a Windows SDK with
`mfvirtualcamera.h`, Python 3.10+, CMake, and vcpkg with dynamic x64 Windows
FFmpeg and Asio dependencies. The existing projects use the Visual Studio vcpkg
integration. wxWidgets 3.3.3 and the pinned inference SDK/assets are bootstrapped
locally by the project's scripts; they require network access on first use.

From the repository root in PowerShell:

```powershell
.\scripts\build-windows.ps1
```

Output is `dist/PrismForgeStudio.exe` and its runtime files. Building does not
register the virtual camera. Camera setup remains a separate administrator action.
When rebuilding an already running app, use MSBuild's `StudioDeployDir` property
to stage it in another directory.

## Android

Install the Android SDK/API levels declared by the Gradle project and a compatible
JDK. Pass your actual SDK path:

```powershell
.\scripts\build-android.ps1 -SdkPath C:\AndroidSDK -RunTests
```

The debug APK is placed in `dist/apk/`. Debug signing is intended for local
development. The public APK is explicitly labelled debug-signed; these commands
do not create a store-signed release or a production signing identity.

## Verification

`tests/` contains focused native suites and Android tests. Use the relevant
suite's `run-*.ps1` script and inspect its actual exit code. A build or synthetic
test does not establish camera compatibility or sustained real-time performance.
Some historical suites reference private/local development fixtures that are
not distributed; obtain your own permitted fixtures before running those cases.

For safe interface inspection, set `PRISMFORGE_UI_PREVIEW=1` for the process.
It disables camera/network services and persisted settings. The isolated native
UI verifier exercises additional fixture-dependent scenarios and closes its
own instance. Private-crown cases need a local permitted asset and are not a
requirement to launch the public bundle.

## Package

```powershell
.\scripts\package.ps1 -OutputPath C:\Builds\PrismForgeV1
```

This inherited development packager refuses an existing output directory and
audits runtime dependencies. For public redistribution, also retain the matching
dependency licenses and FFmpeg source/build recipe, include colour packs and
public guides, and generate a fresh file manifest and SHA-256 checksums. Review
every release payload rather than publishing a whole development folder.

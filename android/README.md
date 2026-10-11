# PrismForge Studio Android companion

Application ID and namespace: `com.prismforge.studio`.
Requires Android API 35 for compilation and Android 9/API 28 or later to run.
Run `../scripts/build-android.ps1 -SdkPath <your SDK path>` from PowerShell.
Debug output is copied to `../dist/apk/prismforge-companion-debug.apk`.
Release builds no longer use the debug signing configuration.

Control defaults to port 7969; RTSP uses 8555. See [build and coexistence notes](../docs/BUILD.md).

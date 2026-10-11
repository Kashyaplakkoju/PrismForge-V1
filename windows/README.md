# PrismForge Studio Windows client

See [development build instructions](../docs/BUILD.md). Run `../scripts/build-windows.ps1` from PowerShell; the script builds the camera library, installer helper and PrismForgeStudio solution in order. Requires Visual Studio C++ v145 and configured vcpkg dependencies.

The Release x64 client is `../dist/PrismForgeStudio.exe`. Registration is a separate administrator action through the PrismForge installer script. Do not register the old Softcam DLL in place of the new camera.

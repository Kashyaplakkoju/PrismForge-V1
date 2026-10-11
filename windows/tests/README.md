# Camera rotation checks

For the portable USB regression, run `python windows/tests/run-adb-tests.py`.
It builds a fake ADB helper in a fresh path containing spaces, an ampersand, and
non-ASCII text. It checks console suppression, reverse/forward/cleanup arguments,
nonzero exits, timeouts, and a missing helper without changing phone mappings.

Run `run-tests.bat` from an x64 Visual Studio Developer Command Prompt. It runs the
rotation validator and the existing quality-profile tests. Invalid rotation
commands must warn and become a zero delta; signed `-90` must retain its meaning.

`canvas_playback.cpp` is an optional visual harness. It sends a captured PNG
through the application's actual `Canvas` and `GuiPreviewScaler`, without
opening network connections or registering a virtual camera. It takes one PNG
path, or loads `fixed-usb-back-off-3-decoded.png` beside its executable.

Example build using the vcpkg x64 Windows dependencies used for this investigation
(adjust library names if your wxWidgets build uses different names):

```bat
mkdir build\tilt-investigation
cl /nologo /std:c++17 /EHsc /MD /DNDEBUG /DWXUSINGDLL /D_UNICODE /DUNICODE /utf-8 ^
  /Iwindows\src /IC:\vcpkg\installed\x64-windows\include ^
  /IC:\vcpkg\installed\x64-windows\lib\mswu ^
  windows\tests\canvas_playback.cpp windows\src\canvas.cpp ^
  windows\src\video\framescaler.cpp windows\src\video\guipreviewscaler.cpp ^
  /Fe:build\tilt-investigation\canvas_playback.exe ^
  /link /SUBSYSTEM:WINDOWS /LIBPATH:C:\vcpkg\installed\x64-windows\lib ^
  wxmsw33u_core.lib wxbase33u.lib avutil.lib swscale.lib gdi32.lib user32.lib
```

Make the corresponding runtime DLLs available beside the executable or on PATH.
The harness is a replay check, not proof of a simultaneous live phone/PC capture.
See [the investigation](../../docs/CAMERA-TILT-INVESTIGATION.md) for capture
commands, real measurements and their limitations.

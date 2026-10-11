<p align="center"><img src="windows/res/prismforge-256.png" width="100" alt="PrismForge Studio"></p>

<h1 align="center">PrismForge Studio V1</h1>
<p align="center"><strong>Your phone camera. Your Windows studio.</strong><br>Connect, frame, light and create—over USB or Wi-Fi.</p>

<p align="center"><a href="https://github.com/Kashyaplakkoju/PrismForge-V1/releases/latest">Download for Windows & Android</a> · <a href="docs/GETTING-STARTED.md">Setup guide</a> · <a href="docs/BUILD.md">Build from source</a> · <a href="https://github.com/Kashyaplakkoju/PrismForge-V1/issues">Feedback</a></p>

![Windows](https://img.shields.io/badge/Windows-11_x64-0078D4?logo=windows)
![Android](https://img.shields.io/badge/Android-Companion-3DDC84?logo=android&logoColor=white)
![Community](https://img.shields.io/badge/Edition-Free_V1-8B7CF8)
![Code license](https://img.shields.io/badge/Code-MIT-22C55E)

PrismForge turns an Android phone into a camera source for Windows. Its desktop
studio adds framing, colour, lighting and background tools before publishing
the result through a virtual camera that compatible meeting and recording apps
can select.

**V1 is available free for education, experimentation and feedback.** This
repository contains the Windows and Android projects, public assets and build
tools. Downloadable Windows and APK files are on the
[Releases page](https://github.com/Kashyaplakkoju/PrismForge-V1/releases).

## Inside the studio

![PrismForge V1 desktop studio](docs/images/studio.png)
*Actual V1 interface in its isolated preview mode. Camera and network services
are disabled for these screenshots; no personal camera image is shown.*

![PrismForge V1 effects controls](docs/images/effects.png)

## What V1 can do

| Workflow | Included |
| --- | --- |
| Connect | Android camera over USB/ADB or Wi-Fi/RTSP; QR-assisted pairing |
| Choose a source | Phone streams and Windows-visible camera/capture devices |
| Frame | Rotation, mirroring, zoom and camera quality controls |
| Create a look | Colour adjustments, local `.cube` LUTs and offline colour packs |
| Light and finish | Studio lighting, face lighting, touch-up and background blur/replacement |
| Add accessories | Glasses, the attributed cap and three original native 3D starter filters |
| Explore your own models | Bounded static `.glb` import with fit and resource-budget controls |
| Send the result | Desktop preview and PrismForge virtual-camera output |

Capabilities depend on the selected source, phone and receiving app. Local
model import supports bounded static conversion; full animation and every glTF
material feature are outside V1's supported scope.

## Start in a few minutes

1. Download **Windows x64** and the **Android companion APK** from Releases.
2. Extract the Windows bundle to a permanent folder. Run `scripts/install.bat`
   as administrator once to register the PrismForge virtual camera.
3. Install the APK on your phone. Grant camera access. For USB, enable USB
   debugging and approve this computer on the phone.
4. Open `PrismForgeStudio.exe`, start the phone's USB or Wi-Fi connection and
   choose the source on Windows.
5. Select **PrismForge Studio** in your meeting or recording app's camera list.

See [the setup and troubleshooting guide](docs/GETTING-STARTED.md)
for Wi-Fi, updates and removal. Registration and removal are explicit actions;
merely downloading this repository performs neither.

## Releases and expectations

This is a **community development release**, supplied as-is. The Windows
executables are unsigned and the companion APK is debug-signed. There is no
guarantee of a particular FPS, device compatibility or latency. Windows and
Android may display installation warnings. Use SHA-256 checksums to verify
downloads; checksums establish file identity, not publisher trust.

The public build is based on the October 10 V1 source checkpoint. It excludes
private user model copies and the unused Standard-licensed visor. The local
Royal crown preset requires its own asset and is not supplied in this release;
you can use your own permitted models through the local-model importer.

PrismForge V2 is developed separately as the paid studio edition, with its
updated interface and presentation workflows. V2 code and packages are not
included in this repository.

## Project map

| Path | Contents |
| --- | --- |
| `windows/` | Native C++/wxWidgets studio, capture and virtual-camera components |
| `android/` | Kotlin Android companion |
| `assets/` | Original starter assets and colour-pack source materials |
| `tests/` | Focused native and Android test code |
| `scripts/`, `tools/` | Build, package and verification tools |
| `docs/` | Setup, build, privacy and release information |

## Credits and licenses

PrismForge builds on [VCamdroid](https://github.com/darusc/VCamdroid),
[Softcam](https://github.com/tshino/softcam), wxWidgets, FFmpeg,
Project Nayuki's QR Code generator, ONNX Runtime and MediaPipe. Their authors'
work and notices are preserved. This is an independently maintained fork.

The software code retains the [MIT license](LICENSE). Assets and dependencies
retain their individual terms; read [asset notices](ASSET-NOTICES.md) and the
notices supplied with the Windows download. MIT-licensed code does not impose
an educational-only restriction.

## Help shape the next iteration

If you try V1, [open an issue](https://github.com/Kashyaplakkoju/PrismForge-V1/issues)
with your Windows version, Android version, connection mode, selected source
and receiving app. Describe what you expected and what happened. Remove
personal images, device identifiers and private addresses from attachments.

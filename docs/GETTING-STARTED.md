# Getting started

Use Windows 11 x64 and an Android device compatible with the supplied companion.
The source project declares its exact Android SDK requirements in
`android/app/build.gradle.kts`. This is a development release; compatibility on
every phone and computer has not been established.

## Windows setup

1. Download the Windows ZIP from the repository's Releases page.
2. Extract the complete folder to a stable location, such as `C:\Apps\PrismForgeV1`.
   Keep the EXE, DLLs, resources, models and bundled tools together.
3. Right-click `scripts/install.bat` and choose **Run as administrator**. This
   explicitly registers the PrismForge camera components. Administrator access
   is needed for this setup action.
4. Launch `PrismForgeStudio.exe` as your normal user.

Do not move or delete the registered folder until you unregister its camera.

## Android and USB

1. Install `PrismForgeStudio-V1-Android-debug.apk` from this release. Android may
   ask you to allow installation from the app you used to open the APK. Grant
   camera permission when PrismForge requests it.
2. Enable Android Developer options and **USB debugging**.
3. Connect a data-capable USB cable. Unlock the phone and approve the computer's
   USB-debugging prompt. Approve only a computer you trust.
4. Open the PrismForge companion and start its USB connection. Choose the phone
   source in the Windows studio.

For a device check, open a terminal in the extracted Windows folder and run
`adb\adb.exe devices`. An `unauthorized` device needs approval on the phone.
Only one phone is targeted by the V1 USB setup flow.

## Wi-Fi

Put the phone and PC on the same trusted local network. Use **Connect phone**
on Windows and the companion's QR pairing, or enter the PC address manually.
The default control port is **7969** and RTSP video port is **8555**. If Windows
Firewall asks, allow the app on the intended private network. Avoid public
port forwarding: V1's local streaming transport does not provide internet
authentication or end-to-end encryption.

## Use the camera in another app

Keep the companion stream and Windows studio running. Open the camera picker
in the receiving app and choose **PrismForge Studio**. Restart the receiving
app if it cached its camera list before setup. Some apps impose their own
resolution or frame-rate restrictions.

## Looks, models and packs

Open the Windows controls to adjust colour, lighting, backgrounds or accessories.
Use master bypass to compare against the original feed. The included starter
colour packs are in `packs/`; its README explains offline import and updates.
Import only model files and packs you trust and are entitled to use.
The privately supplied Royal crown and unused visor are excluded from this
public bundle. Their absence does not prevent the camera or other filters from
running.

## Update or remove

Close receiving apps and PrismForge, then run `scripts/uninstall.bat` as
administrator **from the old folder** before moving or deleting it. Extract the
new release into its own stable folder and run its setup script. Unregistration
does not delete your source or Android data. Keep your settings and imported
packs backed up before an update.

The supplied APK uses development signing. Android will reject an update signed
with a different key. Back up any settings before uninstalling an incompatible
older companion; uninstalling an Android app removes its app data.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Phone missing | Data cable, USB debugging, unlocked phone and ADB authorization |
| No picture | Companion stream started; correct source selected; camera permission granted |
| Wi-Fi cannot connect | Same network, correct PC address and firewall; guest-network client isolation |
| Virtual camera missing | Run the matching setup script; restart receiving app; keep registered folder intact |
| Low FPS | Reduce resolution or expensive effects; compare bypass; check receiving-app limits |
| Missing DLL | Re-extract the complete ZIP; avoid mixing files from different releases |
| Model unavailable | Choose an included model or import your own permitted `.glb` |

Report reproducible issues through this repository. Include software versions
and steps; remove faces, private addresses and device identifiers from logs.

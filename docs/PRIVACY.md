# Privacy and local connections

V1 sends the Android camera feed and control messages to the connected Windows
computer using USB/ADB or the local Wi-Fi network. Desktop effects use app-local
inference assets. The V1 application does not require a PrismForge cloud account
to stream or apply these local effects.

USB requires Android debugging authorization. Approve only trusted computers
and revoke that authorization in Android settings when it is no longer needed.
Local RTSP/control transport is not an authenticated, encrypted internet
service. Use a trusted network and do not expose its ports publicly.

The receiving meeting/recording application has its own permissions, recording
behavior and privacy policy. Stop streaming when finished. Local application
logs may include device identifiers and network addresses; review and redact
them before sharing a bug report. No personal camera photos or development
logs are included in this public release.

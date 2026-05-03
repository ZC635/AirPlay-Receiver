# AirPlay Receiver

Windows desktop AirPlay receiver inspired by LonelyScreen.

The planned MVP receives native iPhone Screen Mirroring over AirPlay, displays the mirrored video, plays synchronized audio, and provides a compact toolbar for volume, always-on-top, settings, and configurable global shortcuts.

## Planned Stack

- Qt/C++ for the Windows desktop application.
- UxPlay/GStreamer for AirPlay mirror and audio receiving.
- Bonjour/mDNS service discovery on Windows.
- GPLv3-compatible distribution because UxPlay is GPLv3.

## Initial Features

- Native iPhone AirPlay Screen Mirroring discovery and connection.
- Mirrored video display in the desktop window.
- Synchronized audio playback.
- Overlay toolbar with volume slider, always-on-top toggle, and settings button.
- Settings page for configurable shortcuts:
  - Toggle always on top.
  - Volume up.
  - Volume down.
  - Toggle toolbar visibility.

## Documentation

- Design: `docs/plans/2026-05-03-airplay-receiver-design.md`

## License

Planned license: GPL-3.0-only, subject to the final integration model and third-party dependency notices.

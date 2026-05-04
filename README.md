# AirPlay Receiver

Windows desktop AirPlay receiver inspired by LonelyScreen. Receives native iPhone Screen Mirroring over AirPlay, displays the mirrored video, plays synchronized audio, and provides a compact toolbar for volume, always-on-top, settings, and configurable global shortcuts.

## Requirements

- Windows 10 or 11
- MSYS2 UCRT64 toolchain (GCC, CMake, Ninja, Qt6)
- GStreamer runtime with base/good/bad/libav plugins
- Bonjour SDK 3.0 and runtime service

See `docs/uxplay-windows-build.md` for exact package versions.

## Build

Quick build (all-in-one):

```powershell
.\scripts\build.ps1           # Configure + build
.\scripts\build.ps1 -Test     # Build + run tests
.\scripts\build.ps1 -Clean    # Wipe build dir first
.\scripts\run.ps1             # Build (if needed) + launch
```

Manual steps:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build-uxplay -G Ninja -DAIRPLAY_WITH_UXPLAY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-uxplay
ctest --test-dir build-uxplay --output-on-failure
```

Default build (without UxPlay):

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the receiver:

```powershell
$env:GST_PLUGIN_PATH = "C:\msys64\ucrt64\lib\gstreamer-1.0"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build-uxplay\airplay_receiver.exe
```

## Features

- Native iPhone AirPlay Screen Mirroring discovery and connection via UxPlay/GStreamer
- Mirrored video display (UxPlay-managed GStreamer window; Qt embedding planned)
- Synchronized audio playback
- Overlay toolbar with volume slider, always-on-top toggle, and settings button
- Settings dialog for configurable shortcuts:
  - Toggle always on top (Ctrl+Alt+T)
  - Volume up (Ctrl+Alt+Up)
  - Volume down (Ctrl+Alt+Down)
  - Toggle toolbar visibility (Ctrl+Alt+B)
- Global hotkey registration via Windows hotkey API
- Dependency diagnostics for missing runtimes

## Documentation

- Design: `docs/plans/2026-05-03-airplay-receiver-design.md`
- Implementation plan: `docs/plans/2026-05-03-airplay-receiver-mvp.md`
- Windows build notes: `docs/uxplay-windows-build.md`
- Manual acceptance: `docs/manual-acceptance.md`
- Third-party dependencies: `third_party/README.md`

## Project Structure

```text
src/
  app/          Qt UI (MainWindow, ToolbarWidget, VideoSurfaceWidget, SettingsDialog)
  backend/      AirPlay receiver facade (AirPlayReceiver, UxPlayReceiver, FakeAirPlayReceiver)
  platform/     Windows-specific hotkey, diagnostics
third_party/
  uxplay/       UxPlay submodule
docs/
  plans/        Design and implementation plans
tests/
  app/          UI tests
  backend/      Backend tests
  platform/     Platform tests
```

## License

GPL-3.0-only, aligned with UxPlay's GPLv3 license.

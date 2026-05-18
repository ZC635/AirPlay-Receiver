# AirPlay Receiver

AirPlay Receiver is a Windows desktop receiver for native iPhone AirPlay Screen Mirroring. It advertises itself over mDNS, receives mirroring streams, displays the mirrored video, plays synchronized audio, and provides a compact toolbar for volume, always-on-top, aspect-ratio, video-fit, settings, and configurable global shortcuts.

This project was developed with assistance from OpenCode, Codex, and DeepSeek. It builds on UxPlay and GStreamer for AirPlay protocol handling and media playback, with a Qt-based Windows desktop interface around the receiver experience.

## Platform Scope

This project is Windows-only. The application, build scripts, tests, and runtime packaging intentionally target Windows 10/11 and may use Windows-native APIs when they produce a better receiver experience. Cross-platform compatibility is not a project goal.

## Quick Start

Download the newest Windows portable build from the [latest release](https://github.com/ZC635/AirPlay-Receiver/releases/latest). Extract the archive, run `airplay_receiver.exe`, then open Control Center on an iPhone, choose Screen Mirroring, and select the advertised receiver name. Keep the iPhone and Windows PC on the same network, and allow the receiver through the Windows firewall if prompted.

## Usage

### Prerequisites

- Windows 10 or 11
- MSYS2 installed; the build script prefers the UCRT64 prefix and can install missing MSYS2 packages, including Qt 6, GStreamer, libplist, OpenSSL, and QMdnsEngine, after confirmation


### Clone

```powershell
git clone --recurse-submodules https://github.com/ZC635/AirPlay-Receiver.git
cd AirPlay-Receiver
```

If you cloned without submodules, initialize them manually:

```powershell
git submodule update --init --recursive
```

### Install Dependencies

Install MSYS2 first. The default location `C:\msys64` works out of the box; if you install it elsewhere, pass `-MSys2Root` or set `AIRPLAY_MSYS2_ROOT` to the UCRT64 prefix.

```powershell
winget install MSYS2.MSYS2
```

The build script installs QMdnsEngine for in-process mDNS discovery. No Bonjour Print Services, iTunes, or Bonjour SDK is required. iPhones discover the receiver over mDNS directly.

### Build And Test

Quick build with UxPlay enabled and dependency bootstrap:

```powershell
.\scripts\build.ps1           # Configure + build
.\scripts\build.ps1 -Test     # Build + run tests
.\scripts\build.ps1 -Clean    # Wipe build dir first
.\scripts\build.ps1 -Deploy   # Build + bundle local runtime files
.\scripts\build.ps1 -All      # Build deployed and portable variants
.\scripts\build.ps1 -Run      # Build + launch
```

Bootstrap options:

```powershell
.\scripts\build.ps1 -SkipInstall                       # Detect only; do not run pacman
.\scripts\build.ps1 -AssumeYes                         # Install missing MSYS2 packages without prompting
.\scripts\build.ps1 -MSys2Root C:\msys64\ucrt64        # Use a specific MSYS2 prefix
```

When MSYS2 packages are missing, the script prints the exact `pacman -S --needed ...` command and asks before installing anything.

Manual UxPlay-enabled build:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build-uxplay -G Ninja -DAIRPLAY_WITH_UXPLAY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-uxplay
ctest --test-dir build-uxplay --output-on-failure
```

Default build without UxPlay:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Portable Build

A portable build is a self-contained deployment that does not require MSYS2 to run. The build bundles the required DLLs, GStreamer plugins, QMdnsEngine, a portable runtime manifest, and a pre-built GStreamer registry into a single directory. In the current build outputs, `build-uxplay-portable\` is about 3.98 MB larger than `build-uxplay\` (427.82 MB vs 423.83 MB). The portable folder can be copied to another Windows machine and run by double-clicking `airplay_receiver.exe`.

```powershell
.\scripts\build.ps1 -Portable   # Build the portable bundle
.\scripts\run.ps1 -Portable     # Build + launch portable
```

Or double-click `airplay_receiver.exe` inside the `build-uxplay-portable\` folder.

### Run

Launch through the helper script:

```powershell
.\scripts\run.ps1
.\scripts\run.ps1 -Deploy   # Refresh bundled runtime before launching
```

When `build-uxplay` contains deployed runtime files, `run.ps1` launches in standalone mode. Otherwise it falls back to the discovered MSYS2 prefix for local development.

Or launch the built receiver directly:

```powershell
$env:AIRPLAY_MSYS2_PATH_MODE = "1"
$env:GST_PLUGIN_PATH = "C:\msys64\ucrt64\lib\gstreamer-1.0"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build-uxplay\airplay_receiver.exe
```

After the receiver starts, open Control Center on an iPhone, choose Screen Mirroring, and select the advertised receiver name. Keep the Windows host and iPhone on the same network, and allow mDNS and receiver traffic through the firewall.

## Features

- Native iPhone AirPlay Screen Mirroring discovery and connection via UxPlay/GStreamer
- Mirrored video display through an appsink-to-`QImage` bridge and a Qt `QWidget`/`QPainter` surface with cached-frame repainting to reduce resize artifacts
- Synchronized audio playback
- Bidirectional volume synchronization between iPhone AirPlay volume callbacks and the toolbar slider
- Overlay toolbar with volume slider, always-on-top toggle, aspect-ratio lock, video-fit toggle, and settings button
- Settings dialog for receiver name, video quality, and configurable shortcuts:
  - Receiver name shown in the iPhone Screen Mirroring list
  - Video quality: 540p, 720p, or 1080p; 15, 30, or 60 fps
  - Toggle always on top (`Ctrl+Alt+T`)
  - Volume up (`Ctrl+Alt+Up`)
  - Volume down (`Ctrl+Alt+Down`)
  - Toggle toolbar visibility (`Ctrl+Alt+B`)
  - Toggle aspect-ratio lock (`Ctrl+Alt+A`)
  - Toggle video fit (`Ctrl+Alt+F`)
  - Reset hotkey bindings to defaults
- Global hotkey registration via the Windows hotkey API
- Windows-native window handling for always-on-top state, decoded-frame aspect-ratio resizing, and reduced resize flicker
- Settings persistence in `airplay-settings.json` beside the executable
- Portable Windows bundle with Qt, GStreamer plugins, QMdnsEngine, a runtime manifest, and a pre-built GStreamer registry
- Standalone dependency diagnostics for missing portable runtime files

## Project Structure

```text
src/
  app/          Qt UI, toolbar, video surface, settings dialog, settings persistence
  backend/      Receiver abstraction and UxPlay/GStreamer integration
  platform/     Windows hotkeys, diagnostics, mDNS helpers, window sizing support
cmake/          UxPlay, QMdnsEngine, and renderer dependency integration
config/         Portable runtime manifest
scripts/        PowerShell build, deploy, portable packaging, and run helpers
docs/           Project overview and maintenance notes
third_party/
  uxplay/       UxPlay submodule
tests/
  app/          UI tests
  backend/      Backend tests
  platform/     Platform tests
  scripts/      PowerShell packaging and run-script tests
```

## License

GPL-3.0-only. See `LICENSE` for the full license text.

Third-party dependencies retain their own licenses. UxPlay is included as a GPLv3 submodule from `https://github.com/ZC635/UxPlay.git`; additional bundled third-party license files are documented under `third_party/README.md` and each dependency directory.

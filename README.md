# AirPlay Receiver

AirPlay Receiver is a Windows desktop receiver for native iPhone AirPlay Screen Mirroring. It discovers and receives AirPlay mirroring streams, displays the mirrored video, plays synchronized audio, and provides a compact toolbar for volume, always-on-top, settings, and configurable global shortcuts.

This project was developed with assistance from OpenCode, Codex, and DeepSeek. It builds on UxPlay and GStreamer for AirPlay protocol handling and media playback, with a Qt-based Windows desktop interface around the receiver experience.

## Usage

### Prerequisites

- Windows 10 or 11
- MSYS2 installed; the build script detects the UCRT64 prefix and can install missing MSYS2 packages after confirmation
- Bonjour SDK 3.0 and Bonjour runtime service


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

Install Bonjour runtime support so iPhones can discover the receiver. Use Bonjour Print Services for Windows from Apple, or install iTunes if that is your preferred Apple runtime package:

```powershell
# Install Bonjour Print Services for Windows or iTunes, then verify the service.
Get-Service "Bonjour Service"
Start-Service "Bonjour Service"   # if installed but stopped
```

Install Bonjour SDK 3.0 so UxPlay can link against DNS-SD during the build. Download `Bonjour SDK for Windows 3.0` from Apple Developer Downloads at `https://developer.apple.com/download/all/?q=Bonjour%20SDK%20for%20Windows`. Apple Developer sign-in may be required. The default SDK install should provide these files:

```text
C:\Program Files\Bonjour SDK\Include\dns_sd.h
C:\Program Files\Bonjour SDK\Lib\x64\dnssd.lib
```

If the SDK is installed elsewhere, set `BONJOUR_SDK_HOME` before building:

```powershell
$env:BONJOUR_SDK_HOME = "D:\Tools\Bonjour SDK"
```

### Build And Test

Quick build with UxPlay enabled and dependency bootstrap:

```powershell
.\scripts\build.ps1           # Configure + build
.\scripts\build.ps1 -Test     # Build + run tests
.\scripts\build.ps1 -Clean    # Wipe build dir first
.\scripts\build.ps1 -Deploy   # Build + bundle local runtime files
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

A portable build is a fully self-contained deployment that does not require MSYS2 to run. The build bundles all required DLLs, GStreamer plugins, and a pre-built GStreamer registry into a single directory. The resulting folder is larger (~200MB+) but can be copied to any Windows machine and run by simply double-clicking `launcher.cmd`.

```powershell
.\scripts\build.ps1 -Portable   # Build the portable bundle
.\scripts\run.ps1 -Portable     # Build + launch portable
```

Or double-click `launcher.cmd` inside the `build-uxplay-portable\` folder.

### Run

Launch through the helper script:

```powershell
.\scripts\run.ps1
.\scripts\run.ps1 -Deploy   # Refresh bundled runtime before launching
```

When `build-uxplay` contains deployed runtime files, `run.ps1` launches in standalone mode. Otherwise it falls back to the discovered MSYS2 prefix for local development.

Or launch the built receiver directly:

```powershell
$env:GST_PLUGIN_PATH = "C:\msys64\ucrt64\lib\gstreamer-1.0"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build-uxplay\airplay_receiver.exe
```

After the receiver starts, open Control Center on an iPhone, choose Screen Mirroring, and select the advertised receiver name. Keep the Windows host and iPhone on the same network, and allow Bonjour/mDNS and receiver traffic through the firewall.

## Features

- Native iPhone AirPlay Screen Mirroring discovery and connection via UxPlay/GStreamer
- Mirrored video display through the UxPlay-managed GStreamer window
- Synchronized audio playback
- Overlay toolbar with volume slider, always-on-top toggle, and settings button
- Settings dialog for receiver name and configurable shortcuts:
  - Receiver name shown in the iPhone Screen Mirroring list
  - Toggle always on top (`Ctrl+Alt+T`)
  - Volume up (`Ctrl+Alt+Up`)
  - Volume down (`Ctrl+Alt+Down`)
  - Toggle toolbar visibility (`Ctrl+Alt+B`)
  - Reset hotkey bindings to defaults
- Global hotkey registration via the Windows hotkey API
- Dependency diagnostics for missing runtimes

## Project Structure

```text
src/
  app/          Qt UI (MainWindow, ToolbarWidget, VideoSurfaceWidget, SettingsDialog)
  backend/      AirPlay receiver facade (AirPlayReceiver, UxPlayReceiver, FakeAirPlayReceiver)
  platform/     Windows-specific hotkey, diagnostics
third_party/
  uxplay/       UxPlay submodule
tests/
  app/          UI tests
  backend/      Backend tests
  platform/     Platform tests
```

## License

GPL-3.0-only. See `LICENSE` for the full license text.

Third-party dependencies retain their own licenses. UxPlay is included as a GPLv3 submodule from `https://github.com/ZC635/UxPlay.git`; additional bundled third-party license files are documented under `third_party/README.md` and each dependency directory.

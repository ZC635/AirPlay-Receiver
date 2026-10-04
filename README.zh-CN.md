# AirPlay Receiver

[English](README.md) | [简体中文](README.zh-CN.md)

AirPlay Receiver 是一个 Windows 桌面接收器，用于原生 iPhone AirPlay 屏幕镜像。它通过 mDNS 广播自身、接收镜像流、显示镜像视频、播放同步音频、将镜像内容录制为 MP4，并提供紧凑工具栏以控制音量、窗口置顶、宽高比、视频适应、录制、设置和可配置全局快捷键。

本项目由 OpenCode、Codex 与 DeepSeek 协助开发。它基于 UxPlay 和 GStreamer 实现 AirPlay 协议处理与媒体播放，并以 Qt Windows 桌面界面承载接收器体验。

## 平台范围

本项目仅支持 Windows。应用程序、构建脚本、测试和运行时打包有意面向 Windows 10/11；当 Windows 原生 API 能带来更好的接收器体验时，项目会使用它们。跨平台兼容性不是本项目目标。

## 语言

默认情况下，应用程序跟随 Windows 系统语言。如果该语言不受支持或缺少翻译，界面将回退为英文。打开 **设置 > 常规 > 语言**，选择 **跟随系统**、**英文** 或 **简体中文**，然后点击 **应用**；已打开的界面会立即切换，无需重启。

未来的本地化文件可以不完整。它们仍会被接受，每个缺失翻译都会显示为英文。

## 快速开始

从[最新发布版](https://github.com/ZC635/AirPlay-Receiver/releases/latest)下载最新 Windows 便携构建。解压存档，运行 `airplay_receiver.exe`，然后在 iPhone 上打开控制中心，选择“屏幕镜像”，再选择已广播的接收器名称。请保持 iPhone 与 Windows PC 位于同一网络；如出现提示，请允许接收器通过 Windows 防火墙。

接收器启动后即可使用默认全局快捷键：`Ctrl+Alt+T` 切换窗口置顶，`Ctrl+Alt+Up` 和 `Ctrl+Alt+Down` 调整音量，`Ctrl+Alt+B` 切换工具栏可见性，`Ctrl+Alt+A` 切换宽高比锁定，`Ctrl+Alt+F` 切换视频适应，`Ctrl+Alt+R` 开始或停止录制。打开工具栏的设置按钮可自定义快捷键绑定，或将它们恢复为默认值。

工具栏使用单色图标。鼠标悬停在按钮上时，提示会显示当前语言的文字及已配置的全局快捷键。

除非已保存其他偏好，宽高比锁定和视频适应默认开启。宽高比锁定会在调整窗口大小时跟随已解码视频的比例；视频适应会保持该比例，并显示完整画面。

点击 **全屏** 图标，或按 `F11`，可切换全屏；按 `Esc` 可退出。这些固定的窗口快捷键仅在主窗口处于活动状态，且没有模态对话框或弹出窗口时生效。退出时恢复此前的窗口大小、位置和最大化状态。镜像会话结束时会退出全屏，下次启动也不会恢复全屏。

镜像连接生效时工具栏会隐藏。`Ctrl+Alt+B` 可切换其可见性；手动显示的工具栏会保持显示，直到再次隐藏或接收器状态发生变化。默认情况下，将鼠标移到活动接收器窗口的内容区域顶部，可在窗口和全屏模式下临时显示隐藏的工具栏。移出该顶部区域、工具栏及其控件后，临时显示的工具栏会再次隐藏。可在 **设置 > 常规 > 工具栏隐藏时，鼠标移到顶部显示** 中更改此行为，然后点击 **应用**。

## 使用

### 前置条件

- Windows 10 或 11
- 已安装 MSYS2；构建脚本优先使用 UCRT64 前缀，并可在确认后安装缺少的 MSYS2 软件包，包括 Qt 6、GStreamer、libplist、OpenSSL 和 QMdnsEngine


### 克隆

```powershell
git clone --recurse-submodules https://github.com/ZC635/AirPlay-Receiver.git
cd AirPlay-Receiver
```

若克隆时未包含子模块，请手动初始化：

```powershell
git submodule update --init --recursive
```

### 安装依赖

先安装 MSYS2。默认路径 `C:\msys64` 可直接使用；若安装在其他位置，请传入 `-MSys2Root`，或将 `AIRPLAY_MSYS2_ROOT` 设置为 UCRT64 前缀。

```powershell
winget install MSYS2.MSYS2
```

构建脚本会安装 QMdnsEngine，以进行进程内 mDNS 发现。不需要 Bonjour Print Services、iTunes 或 Bonjour SDK；iPhone 通过 mDNS 直接发现接收器。

### 构建与测试

启用 UxPlay 并自举依赖的快速构建：

```powershell
.\scripts\build.ps1           # Configure + build
.\scripts\build.ps1 -Test     # Build + run tests
.\scripts\build.ps1 -Clean    # Wipe build dir first
.\scripts\build.ps1 -Deploy   # Build + bundle local runtime files
.\scripts\build.ps1 -All      # Build deployed and portable variants
.\scripts\build.ps1 -Run      # Build + launch
```

自举选项：

```powershell
.\scripts\build.ps1 -SkipInstall                       # Detect only; do not run pacman
.\scripts\build.ps1 -AssumeYes                         # Install missing MSYS2 packages without prompting
.\scripts\build.ps1 -MSys2Root C:\msys64\ucrt64        # Use a specific MSYS2 prefix
```

当 MSYS2 软件包缺失时，脚本会打印精确的 `pacman -S --needed ...` 命令，并在执行安装前询问。

手动启用 UxPlay 构建：

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build-uxplay -G Ninja -DAIRPLAY_WITH_UXPLAY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-uxplay
ctest --test-dir build-uxplay --output-on-failure
```

不启用 UxPlay 的构建：

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DAIRPLAY_WITH_UXPLAY=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

新配置 CMake 时，Windows QPA 原生窗口测试默认开启；若定义了 `CI` 环境变量，则默认关闭。可在配置命令中加入 `-DAIRPLAY_ENABLE_WINDOWS_QPA_TESTS=OFF` 禁用这些测试。Offscreen 测试无法验证实际桌面焦点。

### 便携构建

便携构建是无需 MSYS2 即可运行的自包含部署。构建会将所需 DLL、GStreamer 插件、QMdnsEngine、便携运行时清单和预构建 GStreamer 注册表打包到单个目录中。在当前构建输出中，`build-uxplay-portable\` 比 `build-uxplay\` 大约多 3.98 MB（427.82 MB 对 423.83 MB）。可将便携目录复制到另一台 Windows 计算机，并双击 `airplay_receiver.exe` 运行。

```powershell
.\scripts\build.ps1 -Portable   # Build the portable bundle
.\scripts\run.ps1 -Portable     # Build + launch portable
```

或者双击 `build-uxplay-portable\` 文件夹内的 `airplay_receiver.exe`。

### 诊断

普通启动不会写入诊断日志。要为排障会话收集日志，请在设置中选择 `Restart with Diagnostic Logging`，或从已部署应用程序文件夹显式运行 `Start with Diagnostic Logging.cmd`。`AIRPLAY_DEBUG_LOG` 仅保留为安全模式兼容项。

诊断日志仅写入相邻的 `logs` 目录：每个会话一个文件，每个文件最大 20 MB，保留最新 10 个文件。日志绝不会自动上传；仅在被要求时手动发送。`[Diagnostic Logging]` 标题后缀是诊断日志已启用的唯一持续指示。若相邻目录不可写入，则不会使用备用目录，也不需要管理员权限。诊断日志提供排障信息；它不会自动确定根本原因。

### 运行

通过辅助脚本启动：

```powershell
.\scripts\run.ps1
.\scripts\run.ps1 -Deploy   # Refresh bundled runtime before launching
```

当 `build-uxplay` 包含已部署的运行时文件时，`run.ps1` 会以独立模式启动。否则，它会回退到已发现的 MSYS2 前缀，用于本地开发。

或者直接启动已构建的接收器：

```powershell
$env:AIRPLAY_MSYS2_PATH_MODE = "1"
$env:GST_PLUGIN_PATH = "C:\msys64\ucrt64\lib\gstreamer-1.0"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
.\build-uxplay\airplay_receiver.exe
```

接收器启动后，在 iPhone 上打开控制中心，选择“屏幕镜像”，再选择已广播的接收器名称。请保持 Windows 主机与 iPhone 位于同一网络，并允许 mDNS 和接收器流量通过防火墙。

### 录制镜像内容

当 iPhone 正在镜像且存在可录制样本时，点击工具栏上的圆形 **录制** 图标，或按 `Ctrl+Alt+R`。点击方形 **停止** 图标，或再次按快捷键即可完成。录制正在结束并写入 MP4 文件时，按钮会显示禁用的沙漏图标，悬停提示包含 **正在保存…** 及已配置的快捷键。

默认情况下，录制文件保存在 Windows 视频文件夹下的 `AirPlay Receiver Recording`。打开 **设置 > 录制** 可选择其他输出文件夹，或控制是否显示完成消息。录制进行中或正在保存时关闭应用程序，会在丢弃录制内容前请求确认。

录制管线会在运行时选择可用的 H.264 编码器，优先使用可用的 Windows Media Foundation，否则回退到 OpenH264。便携构建包含所需 GStreamer 录制插件，并在打包时验证它们。

## 功能

- 通过 UxPlay/GStreamer 实现原生 iPhone AirPlay 屏幕镜像发现和连接
- 通过 appsink 到 `QImage` 桥接显示镜像视频，并使用具有缓存帧重绘功能的 Qt `QWidget`/`QPainter` 表面减少调整大小伪影
- 同步音频播放
- 通过 UxPlay 原始样本接入点录制镜像视频和可用音频为 MP4，提供工具栏控制与可配置全局快捷键
- iPhone AirPlay 音量回调与工具栏滑块之间的双向音量同步
- 带有音量滑块、窗口置顶切换、宽高比锁定、视频适应切换、录制控制、全屏切换和设置按钮的覆盖式工具栏
- 默认开启宽高比锁定和视频适应，同时保留已保存的偏好
- 通过工具栏按钮或窗口快捷键 (`F11` / `Esc`) 控制全屏，并在退出时恢复此前的窗口状态
- 可配置的隐藏工具栏悬停显示，在窗口和全屏模式下均可使用，默认开启
- 用于语言、工具栏悬停显示、接收器名称、视频质量、录制输出和可配置快捷键的设置对话框：
  - 在 iPhone“屏幕镜像”列表中显示的接收器名称
  - 视频质量：540p、720p 或 1080p；15、30 或 60 fps
  - 切换窗口置顶 (`Ctrl+Alt+T`)
  - 增加音量 (`Ctrl+Alt+Up`)
  - 降低音量 (`Ctrl+Alt+Down`)
  - 切换工具栏可见性 (`Ctrl+Alt+B`)
  - 切换宽高比锁定 (`Ctrl+Alt+A`)
  - 切换视频适应 (`Ctrl+Alt+F`)
  - 切换录制 (`Ctrl+Alt+R`)
  - 将快捷键绑定恢复为默认值
- 通过 Windows 快捷键 API 注册全局快捷键
- 用于窗口置顶状态、已解码帧宽高比调整和减少调整大小闪烁的 Windows 原生窗口处理
- 将设置持久化到可执行文件旁的 `airplay-settings.json`
- 包含 Qt、GStreamer 插件、QMdnsEngine、运行时清单和预构建 GStreamer 注册表的 Windows 便携包
- 针对缺失便携运行时文件的独立依赖诊断

## 项目结构

```text
src/
  app/          Qt UI, toolbar, video surface, settings dialog, settings persistence
  backend/      Receiver, UxPlay/GStreamer integration, and MP4 recording pipeline
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

## 许可证

GPL-3.0-only。完整许可证文本请参见 `LICENSE`。

第三方依赖保留各自许可证。UxPlay 作为 GPLv3 子模块从 `https://github.com/ZC635/UxPlay.git` 引入；额外打包的第三方许可证文件记录在 `third_party/README.md` 及每个依赖目录中。

# Windows UCRT64 CI

[`windows-ucrt64.yml`](windows-ucrt64.yml) 在 `main` / `master` 的 push、所有
pull request 或手动 dispatch 时运行。Job 使用 `windows-2022`，只申请
`contents: read`，不发布构建。功能分支通过 PR 集成；合入主分支前，必须确认该 PR
当前提交的 **Windows UCRT64** 工作流成功。

## 构建和依赖

1. 保留 LF，递归检出固定版本的子模块，不持久化 Git 凭据。
2. 用 `msys2/setup-msys2` 更新 UCRT64 并安装 GCC，创建工具链前缀。
3. 调用 `scripts/build.ps1 -AssumeYes -InstallOnly`，复用脚本唯一的完整包清单：
   CMake、Ninja、Qt6 base/tools/translations/declarative、pkgconf、OpenSSL、libplist、
   GStreamer、base/good/bad/libav 插件、JSON-GLib 和 OpenH264。
4. 按现有 CMake 定义构建应用、源码内的 QMdnsEngine、UxPlay 和测试。Qt6 与
   GStreamer 使用 MSYS2 的 UCRT64 二进制包，不从源码重编这两个框架。
5. 使用工作区内的可写临时目录和独立 GStreamer 注册表，先预热插件扫描，再串行运行
   CTest，禁止“零项测试也成功”，保存 JUnit、CTest、QtTest 和进程验收日志。
   Artifact 只收集诊断文件，避免上传验收夹具中的整套 DLL 和媒体文件。

参考了 [UxPlay 的成功运行](https://github.com/ZC635/UxPlay/actions/runs/40180725583)
及其 [对应 workflow](https://github.com/ZC635/UxPlay/blob/de4a4129f8905e84892e1631624a7a2eb915b167/.github/workflows/build.yml)。
2026-11-13 通过公开 API 读取到该运行的 UCRT64、MINGW64、CLANGARM64、Linux job
均为 `success`；其中 [UCRT64 job](https://github.com/ZC635/UxPlay/actions/runs/40180725583/job/121131035571)
可单独核对。复用其 GCC/OpenSSL/libplist/GStreamer/pkg-config 依赖方向，Qt6、
录制和运行时插件以本项目脚本为准。

`MINGW_PREFIX` 由 MSYS2 shell 提供，workflow 不假定 MSYS2 安装在 `C:\msys64`。
CTest 原有的两个硬编码 `objdump` 路径也改为跟随已找到的 Qt 工具链。

UxPlay 子目录使用 `EXCLUDE_FROM_ALL`，原先默认构建不能保证生成其全部测试程序。
`airplay_ctest_prerequisites` 从该子目录的 `BUILDSYSTEM_TARGETS` 自动收集测试目标，
加入默认构建，避免 workflow 再维护一份配对、清理和协议回归目标清单。

## 测试边界

| 范围 | 默认 CI | 环境和覆盖限制 |
| --- | --- | --- |
| 诊断、设置、语言、窗口状态、几何、快捷键策略、工具栏和普通窗口测试 | 运行 | 沿用 CTest 的 `offscreen` 设置；不证明真实桌面焦点、全局快捷键或原生堆叠正确 |
| 接收器状态、重连、录制事务/时间线、appsink、样本队列、合成 MP4 和 GStreamer 管线 | 运行 | 使用现有本地夹具和软件插件；不证明 iPhone 投屏、声卡或硬件编码正常 |
| GStreamer 缓存、插件缺失、子进程隔离和启动提示验收 | 运行 | 脚本已有 `offscreen` 探针；需要 Windows 进程、文件锁、ACL 等系统功能，但无需交互桌面 |
| UxPlay 配对、Setup 长度、内存、清理、NTP、NAL、HTTP 及样本回调回归 | 运行 | 复用上游 CTest；部分通信测试使用本机 TCP，因此环境仍需允许 loopback |
| `MdnsNetwork_*` 的 17 个场景 | 运行 | 真实 DNS 编解码与发布逻辑，内存网络边界；不需要真实网卡或组播 |
| DNS-SD DirectImport / TransitiveImport / DynamicLoad 负向测试 | 运行 | DLL 导入/加载拒绝；DynamicLoad 在真实服务启动前即被拒绝 |
| `DnssdRuntimeAcceptanceTest` | 排除，标记 `network` | 需要实际网卡、UDP 5353、组播加入及本机回环接收；无需交互桌面，但不适合作为托管 runner 的固定网络前提 |
| 下列 10 项原生窗口测试 | 默认不登记；启用时标记 `windows-desktop` | 需要真实 Windows QPA 和可交互桌面 |

真实桌面测试保留为：

- `MainWindowTopmostFlickerTest`
- `MainWindowNativeAspectSizingTest`
- `MainWindowNativeLeftAspectSizingTest`
- `MainWindowSmokeTestNativeOverlayStacking`
- `MainWindowNativeFullscreenKeysTest`
- `MainWindowNativeFullscreenAspectTest`
- `MainWindowNativeFullscreenTopmostTest`
- `MainWindowNativeToolbarHoverTest`
- `ToolbarWidgetNativeCheckedColorsTest`
- `ToolbarWidgetNativeCheckedColorsHighDpiTest`

标签从现有 `QT_QPA_PLATFORM=windows` 测试属性生成，workflow 不重复列出测试名。
默认 CI 显式配置 `AIRPLAY_ENABLE_WINDOWS_QPA_TESTS=OFF`，再用
`--label-exclude 'windows-desktop|network'` 排除受环境约束的验收。

本地真实桌面检查（以下命令本次未执行原生窗口测试）：

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build-uxplay -G Ninja -DCMAKE_BUILD_TYPE=Release -DAIRPLAY_WITH_UXPLAY=ON -DBUILD_TESTING=ON -DAIRPLAY_ENABLE_WINDOWS_QPA_TESTS=ON
cmake --build build-uxplay --parallel 2
ctest --test-dir build-uxplay -L windows-desktop --parallel 1 --output-on-failure --no-tests=error
```

在允许真实组播的网络上可另行运行 `ctest --test-dir build-uxplay -L network --output-on-failure`。
双网卡、路由优先级变化、防火墙、iPhone 发现/配对/投屏、实际音视频和手工输入仍需按
[`tests/mdns/README.md`](../../tests/mdns/README.md) 做真实设备验收。

## 验证记录（2026-11-13）

以下记录对应创建 PR 前的本地验证阶段。后续托管 runner 的结果以 PR 的检查记录为准。
本机为 Windows，已有 MSYS2 UCRT64、GCC 16.1.0、CMake/CTest 4.3.2、
Qt6 6.11.0 和 GStreamer 1.28.2。验证输出保存在仓库忽略的 `out/ci-ucrt64/`。
代码基线为 v0.4.4 提交 `60e558efe1adce19fbf5538a18989069590aba1b`。

- actionlint 1.7.12、YAML 解析和 PowerShell 语法检查通过。
- `-InstallOnly -SkipInstall` 在 PowerShell 和 MSYS2 shell 中通过，不触发配置/编译；
  `-InstallOnly -Test` 按预期拒绝冲突参数。
- 新目录 CMake 配置和完整编译通过，包括应用、QMdnsEngine 和 UxPlay 全部测试程序。
  从 YAML 提取的 MSYS2 配置与构建命令也已实际执行。
- 配置元数据检查通过：119 项默认 CI 测试、10 项桌面测试、1 项真实网络测试。
  开启或关闭原生 QPA 开关后，标签筛选得到相同的 119 项默认 CI 测试。
- 首次本地 MSYS2 测试遇到 `C:\msys64\tmp` 不可写，导致 Qt 临时目录断言失败；
  同一设置测试改用工作区临时目录后，44 个内部用例全部通过。workflow 已固定可写的
  `TMP` / `TEMP` / `TMPDIR` 和独立 GStreamer 注册表，并预热插件扫描。
- 完整本地离屏 CTest 执行 119 项，耗时 1279.72 秒：118 项通过，1 项失败。
  唯一失败为 `GStreamerCacheStorageTest::nativePackageAliasesHaveOneIdentity`：
  原生 `GetShortPathNameW` 在当前沙箱中返回 Win32 5（访问拒绝）。
  随后在沙箱外通过原 CTest 入口单独重跑 `GStreamerCacheStorageTest`，1/1 通过；
  直接运行该测试的完整 QtTest 也得到 35 个内部用例通过、0 失败。
  未跳过或修改该测试，也未将受限环境的整套结果写成“119/119 全绿”。
- 17 项 `MdnsNetwork_*` 全部通过。合成 MP4 中的 Media Foundation GPU 子用例按
  现有条件跳过；OpenH264 覆盖用例通过。
- 完整运行的 JUnit 为 `out/ci-ucrt64/ctest-results.xml`，完整日志副本为
  `out/ci-ucrt64/headless-suite/LastTest.log`；沙箱外单项重跑的 JUnit 为
  `out/ci-ucrt64/cache-storage-rerun.xml`，QtTest 文本为
  `out/ci-ucrt64/cache-storage-unrestricted.txt`。
- 干净机器的自动安装、GitHub 托管 Windows job、真实桌面/组播/iPhone 验收未执行。
  本地已有依赖的检查与构建不能代替这些验证，也不构成远程 CI 已通过的证据。

## PR 远程验证

[PR #13 的首次运行](https://github.com/ZC635/AirPlay-Receiver/actions/runs/43326737659)
在安装依赖后暴露了固定 OpenH264 DLL ABI 的问题：MSYS2 已安装 OpenH264 2.7.0，
旧脚本仍要求 `libopenh264-7.dll`。开发依赖检测改为识别实际存在的版本化 DLL，
并在现有 `RunScriptTest` 中验证新 ABI、旧 ABI 和缺少运行时的情况。

[第二次完整运行](https://github.com/ZC635/AirPlay-Receiver/actions/runs/43327885408)
完成了依赖安装、配置和构建，119 项测试中有 115 项通过。失败项的 QtTest 文本报告由
[单独的诊断运行](https://github.com/ZC635/AirPlay-Receiver/actions/runs/43332431193) 保存。
诊断测试根据本机时区计算文件名，录制测试显式使用 OpenH264 验证编码顺序和 MP4 生成，
文件事务测试则在专用临时目录内验证 Windows 实际拒绝写入文件属性，再检查清理行为。
这些改动保留了所有原有 CTest 项。完整结果以 PR 当前提交的 **Windows UCRT64** 检查为准。

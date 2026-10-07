<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN" sourcelanguage="en">
<context>
    <name>DiagnosticActivation</name>
    <message>
        <location filename="../src/app/DiagnosticActivation.cpp" line="204"/>
        <source>Diagnostic logging could not be initialized.</source>
        <translation>无法初始化诊断日志。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticActivation.cpp" line="205"/>
        <source>Diagnostic logging could not be initialized: %1</source>
        <translation>无法初始化诊断日志：%1</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticActivation.cpp" line="215"/>
        <source>Diagnostic parent process is unavailable.</source>
        <translation>诊断父进程不可用。</translation>
    </message>
</context>
<context>
    <name>DiagnosticLogFolderActions</name>
    <message>
        <location filename="../src/platform/DiagnosticLogFolderActions.cpp" line="18"/>
        <source>Could not create diagnostic log folder: %1</source>
        <translation>无法创建诊断日志文件夹：%1</translation>
    </message>
    <message>
        <location filename="../src/platform/DiagnosticLogFolderActions.cpp" line="27"/>
        <source>Could not open diagnostic log folder: %1</source>
        <translation>无法打开诊断日志文件夹：%1</translation>
    </message>
</context>
<context>
    <name>DiagnosticRestartCoordinator</name>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="69"/>
        <source>Diagnostic restart timed out waiting for the child process.</source>
        <translation>等待子进程时诊断重启超时。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="82"/>
        <source>Diagnostic restart could not be started.</source>
        <translation>无法启动诊断重启。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="88"/>
        <source>Diagnostic restart could not create its private handoff channel.</source>
        <translation>诊断重启无法创建专用交接通道。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="100"/>
        <source>Diagnostic restart could not start the child process.</source>
        <translation>诊断重启无法启动子进程。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="115"/>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="120"/>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="142"/>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="156"/>
        <source>Diagnostic restart received an invalid child response.</source>
        <translation>诊断重启收到无效的子进程响应。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="126"/>
        <source>Diagnostic restart received an incomplete child response.</source>
        <translation>诊断重启收到不完整的子进程响应。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="135"/>
        <source>Diagnostic restart received an oversized child response.</source>
        <translation>诊断重启收到过大的子进程响应。</translation>
    </message>
    <message>
        <location filename="../src/app/DiagnosticRestartCoordinator.cpp" line="152"/>
        <source>Diagnostic child could not initialize logging.</source>
        <translation>诊断子进程无法初始化日志。</translation>
    </message>
</context>
<context>
    <name>LanguageManager</name>
    <message>
        <location filename="../src/app/LanguageManager.cpp" line="13"/>
        <source>Translation loaded</source>
        <translation>翻译已加载</translation>
    </message>
</context>
<context>
    <name>MainWindow</name>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="403"/>
        <source>RTSS is running and a runtime DLL path is long. This may trigger a third-party compatibility issue and cause the application to exit unexpectedly.

Consider moving the entire application folder to a shorter path, such as C:\AirPlay; or add an application profile in RTSS for %1, set Application detection level to None for this application only, keep the Global settings unchanged, and restart AirPlay.

These steps may reduce the risk, but do not guarantee a successful startup or prevent every crash.</source>
        <translation>检测到 RTSS 正在运行，当前运行库 DLL 路径较长，可能触发第三方兼容性问题，导致程序异常退出。

建议将整个应用文件夹移至较短路径，例如 C:\AirPlay；或在 RTSS 中为 %1 添加独立应用配置，仅将本应用的 Application detection level 设置为 None，保留 Global 设置，然后重新启动 AirPlay。

这些操作可能降低风险，但不能保证启动成功或消除所有崩溃。</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="408"/>
        <source>Dismiss</source>
        <translation>关闭提示</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="92"/>
        <location filename="../src/app/MainWindow.cpp" line="99"/>
        <location filename="../src/app/MainWindow.cpp" line="106"/>
        <source>Diagnostic Logging</source>
        <translation>诊断日志</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="113"/>
        <source>Diagnostic Restart Unavailable</source>
        <translation>诊断重启暂不可用</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="119"/>
        <source>Diagnostic Restart Failed</source>
        <translation>诊断重启失败</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="421"/>
        <source> [Diagnostic Logging]</source>
        <translation> [诊断日志]</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="434"/>
        <location filename="../src/app/MainWindow.cpp" line="444"/>
        <source>Ready for AirPlay</source>
        <translation>等待 AirPlay 连接</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="437"/>
        <source>Connecting</source>
        <translation>正在连接</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="440"/>
        <source>Connected</source>
        <translation>已连接</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="445"/>
        <source>Receiver error: %1</source>
        <translation>接收器错误：%1</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="448"/>
        <source>Could not save settings</source>
        <translation>无法保存设置</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="451"/>
        <source>No recordable mirrored content</source>
        <translation>没有可录制的镜像内容</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="455"/>
        <source>Could not start recording</source>
        <translation>无法开始录制</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="456"/>
        <source>Could not start recording: %1</source>
        <translation>无法开始录制：%1</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="645"/>
        <source>Unknown error.</source>
        <translation>未知错误。</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="653"/>
        <source>Could not register shortcuts: %1</source>
        <translation>无法注册快捷键：%1</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="806"/>
        <source>Diagnostic restart is unavailable while a recording is active or being finalized. Finish or discard the recording, then try again.</source>
        <translation>录制进行中或正在完成保存时，诊断重启不可用。请完成或丢弃录制后重试。</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="811"/>
        <source>Diagnostic logging records application, Windows, and privacy-filtered network information. Logs stay on this computer and are never uploaded automatically.</source>
        <translation>诊断日志会记录应用、Windows 以及经过隐私过滤的网络信息。日志仅保存在本机，不会自动上传。</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="816"/>
        <source>Settings has unapplied changes. Restarting will discard them. Continue?</source>
        <translation>设置中有未应用的更改。重启会丢弃这些更改。继续吗？</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="821"/>
        <source>Restarting disconnects the current mirroring session. Continue?</source>
        <translation>重启会断开当前镜像会话。继续吗？</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="843"/>
        <source>Apply receiver configuration</source>
        <translation>应用接收器配置</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="844"/>
        <source>Applying receiver configuration now will disconnect the connected device.</source>
        <translation>现在应用接收器配置会断开已连接的设备。</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="846"/>
        <source>Disconnect and apply now</source>
        <translation>断开并立即应用</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="847"/>
        <source>Apply after disconnect</source>
        <translation>断开后应用</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="868"/>
        <source>%1: attempted %2. %3</source>
        <translation>%1：尝试值 %2。%3</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="872"/>
        <source> Rollback succeeded.</source>
        <translation> 已回滚成功。</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="876"/>
        <source> Recovery failed: %1</source>
        <translation> 恢复失败：%1</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="878"/>
        <source> Recovery could not be confirmed.</source>
        <translation> 无法确认恢复结果。</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="883"/>
        <source>Deferred receiver configuration failed</source>
        <translation>延迟的接收器配置失败</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1046"/>
        <source>Recording failed</source>
        <translation>录制失败</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1047"/>
        <source>Recording failed for an unknown reason</source>
        <translation>录制失败，原因未知</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1048"/>
        <source>Recording failed: %1</source>
        <translation>录制失败：%1</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1062"/>
        <source>Recording saved to:
%1

%2</source>
        <translation>录制已保存到：
%1

%2</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1063"/>
        <source>Recording saved to:
%1</source>
        <translation>录制已保存到：
%1</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1065"/>
        <source>Recording saved with warning</source>
        <translation>录制已保存，但有警告</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1065"/>
        <source>Recording saved</source>
        <translation>录制已保存</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1067"/>
        <source>Open Folder</source>
        <translation>打开文件夹</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1074"/>
        <source>Could not open recording</source>
        <translation>无法打开录制文件</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1075"/>
        <source>Could not open recording: %1</source>
        <translation>无法打开录制文件：%1</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1087"/>
        <source>Discard recording?</source>
        <translation>丢弃录制？</translation>
    </message>
    <message>
        <location filename="../src/app/MainWindow.cpp" line="1088"/>
        <source>A recording is still active or being saved. Discard it and exit?</source>
        <translation>录制仍在进行或正在保存。要丢弃并退出吗？</translation>
    </message>
</context>
<context>
    <name>RecordingPathActions</name>
    <message>
        <location filename="../src/platform/RecordingPathActions.cpp" line="43"/>
        <source>Choose Recording Directory</source>
        <translation>选择录制目录</translation>
    </message>
    <message>
        <location filename="../src/platform/RecordingPathActions.cpp" line="52"/>
        <source>Could not create recording directory: %1</source>
        <translation>无法创建录制目录：%1</translation>
    </message>
    <message>
        <location filename="../src/platform/RecordingPathActions.cpp" line="61"/>
        <source>Could not open recording directory: %1</source>
        <translation>无法打开录制目录：%1</translation>
    </message>
    <message>
        <location filename="../src/platform/RecordingPathActions.cpp" line="74"/>
        <source>Could not reveal recording file: %1</source>
        <translation>无法显示录制文件：%1</translation>
    </message>
</context>
<context>
    <name>SettingsApplyCoordinator</name>
    <message>
        <source>Settings application was interrupted. Settings were saved, but their final saved state could not be confirmed. Saving failed for %1: %2. Apply again.</source>
        <translation>设置应用已中断。设置已保存，但无法确认最终保存状态。保存到 %1 失败：%2。请重新应用。</translation>
    </message>
    <message>
        <source>Settings application was interrupted before saving. Settings could not be saved to %1: %2. Apply again.</source>
        <translation>设置应用在保存前已中断。无法保存设置到 %1：%2。请重新应用。</translation>
    </message>
    <message>
        <source>Settings application was interrupted. Settings were saved, but their final saved state could not be confirmed. Apply again.</source>
        <translation>设置应用已中断。设置已保存，但无法确认最终保存状态。请重新应用。</translation>
    </message>
    <message><source>Settings were saved, but their final saved state could not be confirmed. Apply again.</source><translation>设置已保存，但无法确认最终保存状态。请重新应用。</translation></message>
    <message>
        <source>Settings are being applied. Try again after the current operation finishes.</source>
        <translation>正在应用设置。请在当前操作结束后重试。</translation>
    </message>
    <message>
        <source>Settings application was interrupted. Saved changes are kept.</source>
        <translation>设置应用已中断。已保存的改动保留。</translation>
    </message>
    <message>
        <source>Settings application was interrupted before saving.</source>
        <translation>设置应用在保存前中断。</translation>
    </message>
    <message>
        <source>Receiver state changed. Apply again to choose when to apply receiver settings.</source>
        <translation>接收器状态已变化。请重新点击应用，选择接收器设置的应用时机。</translation>
    </message>
    <message>
        <source>Receiver settings have not been applied. The saved configuration is kept for the next receiver start.</source>
        <translation>接收器设置尚未应用。已保存配置保留至下次启动接收服务。</translation>
    </message>
    <message>
        <source>Receiver settings are saved for the next receiver start.</source>
        <translation>接收器设置已保存，留待下次启动接收服务。</translation>
    </message>
    <message>
        <source>The receiver is unavailable.</source>
        <translation>接收器不可用。</translation>
    </message>
    <message>
        <source>Receiver preparation requires an idle receiver.</source>
        <translation>准备接收器配置需要接收器处于空闲状态。</translation>
    </message>
    <message>
        <source>Receiver preparation did not complete.</source>
        <translation>接收器配置准备未完成。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="92"/>
        <source>Shortcut registration failed: %1</source>
        <translation>快捷键注册失败：%1</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="94"/>
        <source>Shortcut registration failed.</source>
        <translation>快捷键注册失败。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="100"/>
        <source>Shortcut registration failed: %1. Previous shortcut was restored.</source>
        <translation>快捷键注册失败：%1。已恢复上一个快捷键。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="102"/>
        <source>Shortcut registration failed. Previous shortcut was restored.</source>
        <translation>快捷键注册失败。已恢复上一个快捷键。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="111"/>
        <source>Shortcut restoration could not be confirmed.</source>
        <translation>无法确认快捷键恢复结果。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="117"/>
        <source>Shortcut restoration failed: %1 (native error %2)</source>
        <translation>快捷键恢复失败：%1（原生错误 %2）</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="121"/>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="728"/>
        <source>Shortcut restoration failed: %1</source>
        <translation>快捷键恢复失败：%1</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="235"/>
        <source>Receiver configuration will be applied when its blocker clears.</source>
        <translation>接收器配置会在阻塞解除后应用。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="260"/>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="316"/>
        <source>Receiver configuration could not be applied: %1</source>
        <translation>无法应用接收器配置：%1</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="269"/>
        <source>Receiver restoration could not be confirmed.</source>
        <translation>无法确认接收器恢复结果。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="270"/>
        <source>Receiver restoration failed: %1</source>
        <translation>接收器恢复失败：%1</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="290"/>
        <source>Receiver configuration could not be applied: %1. Previous receiver configuration was restored.</source>
        <translation>无法应用接收器配置：%1。已恢复上一个接收器配置。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="323"/>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="383"/>
        <source>Compensating settings save failed: %1</source>
        <translation>恢复设置时保存失败：%1</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="346"/>
        <source>Receiver configuration did not start. The saved state was restored.</source>
        <translation>接收器配置未启动。已恢复保存的状态。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="352"/>
        <source>Receiver runtime state could not be confirmed.</source>
        <translation>无法确认接收器运行时状态。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="376"/>
        <source>Receiver configuration did not start. The requested settings were saved, but restoring them failed.</source>
        <translation>接收器配置未启动。已保存请求的设置，但恢复这些设置失败。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="455"/>
        <source>Receiver name cannot be empty.</source>
        <translation>接收器名称不能为空。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="468"/>
        <source>Shortcut cannot be empty.</source>
        <translation>快捷键不能为空。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="474"/>
        <source>Shortcut must use a single key combination.</source>
        <translation>快捷键必须使用单个按键组合。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="480"/>
        <source>Shortcut is not supported as a Windows global hotkey.</source>
        <translation>该快捷键不支持作为 Windows 全局热键。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="498"/>
        <source>Shortcut %1 is assigned to more than one action.</source>
        <translation>快捷键 %1 被分配给多个操作。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="722"/>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="741"/>
        <source>Not committed because the settings file could not be saved.</source>
        <translation>未应用，因为无法保存设置文件。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyCoordinator.cpp" line="727"/>
        <source>Shortcut restoration failed.</source>
        <translation>快捷键恢复失败。</translation>
    </message>
</context>
<context>
    <name>SettingsDialog</name>
    <message><source>Could not confirm saved settings for %1: %2</source><translation>无法确认 %1 的保存状态：%2</translation></message>
    <message><source>The final saved settings could not be confirmed. Apply again.</source><translation>无法确认最终保存的设置。请重新应用。</translation></message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="322"/>
        <source>Settings</source>
        <translation>设置</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="323"/>
        <source>General</source>
        <translation>常规</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="324"/>
        <source>Video</source>
        <translation>视频</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="325"/>
        <source>Recording</source>
        <translation>录制</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="326"/>
        <source>Hotkey Binding</source>
        <translation>快捷键绑定</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="327"/>
        <source>Diagnostics</source>
        <translation>诊断</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="329"/>
        <source>Receiver name</source>
        <translation>接收器名称</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="330"/>
        <source>Language</source>
        <translation>语言</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="331"/>
        <source>Resolution</source>
        <translation>分辨率</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="332"/>
        <source>Frame rate</source>
        <translation>帧率</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="333"/>
        <source>Format</source>
        <translation>格式</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="334"/>
        <source>Output folder</source>
        <translation>输出文件夹</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="335"/>
        <source>Show a message when recording completes</source>
        <translation>录制完成时显示消息</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="336"/>
        <source>Show hidden toolbar when the pointer reaches the top</source>
        <translation>工具栏隐藏时，鼠标移到顶部显示</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="338"/>
        <source>Choose...</source>
        <translation>选择…</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="339"/>
        <source>Open</source>
        <translation>打开</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="340"/>
        <source>Reset to Defaults</source>
        <translation>恢复默认值</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="341"/>
        <source>Restart with Diagnostic Logging</source>
        <translation>启用诊断日志并重启</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="342"/>
        <source>Open Log Folder</source>
        <translation>打开日志文件夹</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="343"/>
        <source>Cancel</source>
        <translation>取消</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="344"/>
        <source>Apply</source>
        <translation>应用</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="346"/>
        <source>Action</source>
        <translation>操作</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="347"/>
        <source>Shortcut</source>
        <translation>快捷键</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="348"/>
        <source>Status</source>
        <translation>状态</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="366"/>
        <source>System Default</source>
        <translation>跟随系统</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="446"/>
        <source>%1 (%2): %3</source>
        <translation>%1（%2）：%3</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="451"/>
        <source> (native error %1)</source>
        <translation>（原生错误 %1）</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="456"/>
        <source> Recovery failed: %1</source>
        <translation> 恢复失败：%1</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="458"/>
        <source> Previous setting was restored.</source>
        <translation> 已恢复上一个设置。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="460"/>
        <source> Recovery could not be confirmed.</source>
        <translation> 无法确认恢复结果。</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="520"/>
        <source>Could not save %1: %2 No changes from this Apply were committed.</source>
        <translation>无法保存 %1：%2 本次应用的更改均未生效。</translation>
    </message>
    <message numerus="yes">
        <location filename="../src/app/SettingsDialog.cpp" line="529"/>
        <source>Recovery requires attention; issue count: %n. Previous settings could not be confirmed.</source>
        <translation>
            <numerusform>恢复需要处理；问题数量：%n。无法确认之前的设置。</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../src/app/SettingsDialog.cpp" line="541"/>
        <source>Apply incomplete; setting count: %n. Correct the highlighted fields.</source>
        <translation>
            <numerusform>应用未完成；设置数量：%n。请修正高亮字段。</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../src/app/SettingsDialog.cpp" line="544"/>
        <source>Some settings were applied. </source>
        <translation>部分设置已应用。</translation>
    </message>
    <message numerus="yes">
        <location filename="../src/app/SettingsDialog.cpp" line="548"/>
        <source>Unapplied change count: %n.</source>
        <translation>
            <numerusform>未应用更改数量：%n。</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>SettingsFields</name>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="186"/>
        <source>Receiver name</source>
        <translation>接收器名称</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="188"/>
        <source>Language</source>
        <translation>语言</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="190"/>
        <source>Resolution</source>
        <translation>分辨率</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="192"/>
        <source>Frame rate</source>
        <translation>帧率</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="196"/>
        <source>Shortcut</source>
        <translation>快捷键</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="198"/>
        <source>Format</source>
        <translation>格式</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="200"/>
        <source>Output folder</source>
        <translation>输出文件夹</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="202"/>
        <source>Show a message when recording completes</source>
        <translation>录制完成时显示消息</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="205"/>
        <source>Show hidden toolbar when the pointer reaches the top</source>
        <translation>工具栏隐藏时，鼠标移到顶部显示</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="226"/>
        <source>Enabled</source>
        <translation>已启用</translation>
    </message>
    <message>
        <location filename="../src/app/SettingsApplyTypes.cpp" line="227"/>
        <source>Disabled</source>
        <translation>已禁用</translation>
    </message>
</context>
<context>
    <name>ShortcutActions</name>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="8"/>
        <source>Toggle always on top</source>
        <translation>切换窗口置顶</translation>
    </message>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="10"/>
        <source>Volume up</source>
        <translation>增大音量</translation>
    </message>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="12"/>
        <source>Volume down</source>
        <translation>减小音量</translation>
    </message>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="14"/>
        <source>Toggle toolbar</source>
        <translation>切换工具栏</translation>
    </message>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="16"/>
        <source>Toggle aspect ratio</source>
        <translation>切换宽高比锁定</translation>
    </message>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="18"/>
        <source>Toggle video fit</source>
        <translation>切换视频适应</translation>
    </message>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="20"/>
        <source>Toggle recording</source>
        <translation>切换录制</translation>
    </message>
    <message>
        <location filename="../src/app/ShortcutActionText.cpp" line="22"/>
        <source>Shortcut</source>
        <translation>快捷键</translation>
    </message>
</context>
<context>
    <name>Startup</name>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="37"/>
        <source>Checking playback components…</source>
        <translation>正在检查播放组件…</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="67"/>
        <source>Updating playback component cache…</source>
        <translation>正在更新播放组件缓存…</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="94"/>
        <source>Playback component cache</source>
        <translation>播放组件缓存</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="88"/>
        <source>Playback component cache could not be updated. Required playback component checks passed, so AirPlay will continue starting. Reason: %1. You can exit this application, redeploy the application folder, and try again.</source>
        <translation>播放组件缓存未能更新。当前必需播放组件检查已通过，AirPlay 将继续启动。原因：%1。可在退出本应用后重新部署应用文件夹，再重试。</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="86"/>
        <source>Playback component cache was updated, but its validation record could not be saved. The next startup may need to check it again. Reason: %1.</source>
        <translation>播放组件缓存已更新，但验证记录未能保存。下次启动可能需要重新检查。原因：%1。</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="16"/>
        <source>another application is updating the cache</source>
        <translation>另一个应用正在更新缓存</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="17"/>
        <source>the application folder cannot be written to</source>
        <translation>无法写入应用文件夹</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="18"/>
        <source>temporary storage is unavailable</source>
        <translation>临时存储不可用</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="19"/>
        <source>the component check timed out</source>
        <translation>组件检查超时</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="20"/>
        <source>application files changed during the check</source>
        <translation>检查期间应用文件发生变化</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="21"/>
        <source>the validation record could not be saved</source>
        <translation>无法保存验证记录</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="22"/>
        <source>temporary files could not be cleaned up</source>
        <translation>无法清理临时文件</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="23"/>
        <source>cache file ownership could not be verified</source>
        <translation>无法验证缓存文件归属</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="24"/>
        <source>the cache could not be replaced safely</source>
        <translation>无法安全替换缓存</translation>
    </message>
    <message>
        <location filename="../src/app/GStreamerStartupPresentation.cpp" line="27"/>
        <source>the playback component cache could not be verified</source>
        <translation>无法验证播放组件缓存</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="98"/>
        <source>Diagnostic log may be incomplete: %1</source>
        <translation>诊断日志可能不完整：%1</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="117"/>
        <source>Diagnostic log could not be fully saved: %1</source>
        <translation>诊断日志未能完整保存：%1</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="75"/>
        <source>Diagnostic logging could not be started.</source>
        <translation>无法启动诊断日志。</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="78"/>
        <source>Diagnostic logging could not be started: %1</source>
        <translation>无法启动诊断日志：%1</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="84"/>
        <source>Diagnostic logging stopped.</source>
        <translation>诊断日志已停止。</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="86"/>
        <source>Diagnostic logging stopped: %1</source>
        <translation>诊断日志已停止：%1</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="121"/>
        <source>Diagnostic logging stopped</source>
        <translation>诊断日志已停止</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="169"/>
        <source>Unsupported application path</source>
        <translation>不支持的应用路径</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="170"/>
        <source>The current application folder uses characters unsupported by the current Windows system language. Bundled GStreamer plugins cannot load from this location.

Move the entire extracted application folder to a short path containing only English letters, numbers, spaces, hyphens, and underscores (for example, C:\AirPlay), then restart the application.</source>
        <translation>当前应用文件夹包含当前 Windows 系统语言不支持的字符。捆绑的 GStreamer 插件无法从此位置加载。

请将整个解压后的应用文件夹移动到仅包含英文字母、数字、空格、连字符和下划线的短路径（例如 C:\AirPlay），然后重启应用。</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="195"/>
        <location filename="../src/app/main.cpp" line="201"/>
        <source>GStreamer plugins unavailable</source>
        <translation>GStreamer 插件不可用</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="196"/>
        <source>Required GStreamer plugins could not be loaded from the current application folder:

%1

Move the entire extracted folder to a short path containing only English letters, numbers, spaces, hyphens, and underscores, for example C:\AirPlay, then restart the application.</source>
        <translation>无法从当前应用文件夹加载必需的 GStreamer 插件：

%1

请将整个解压后的文件夹移动到仅包含英文字母、数字、空格、连字符和下划线的短路径，例如 C:\AirPlay，然后重启应用。</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="202"/>
        <source>Required GStreamer plugins could not be loaded:

%1

Re-extract the portable package. If the problem persists, restart with diagnostic logging and report the generated log.</source>
        <translation>无法加载必需的 GStreamer 插件：

%1

请重新解压便携包。如果问题仍然存在，请使用诊断日志重启并报告生成的日志。</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="112"/>
        <source>Diagnostic logging unavailable</source>
        <translation>诊断日志不可用</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="488"/>
        <source>AirPlay Receiver dependencies missing</source>
        <translation>缺少 AirPlay Receiver 依赖</translation>
    </message>
    <message>
        <location filename="../src/app/main.cpp" line="490"/>
        <source>This standalone build is missing required runtime files:

%1

Run scripts\build.ps1 -Deploy, then launch airplay_receiver.exe again.</source>
        <translation>此独立构建缺少必需的运行时文件：

%1

请运行 scripts\build.ps1 -Deploy，然后再次启动 airplay_receiver.exe。</translation>
    </message>
</context>
<context>
    <name>ToolbarWidget</name>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="206"/>
        <source>%1: %2</source>
        <translation>%1：%2</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="196"/>
        <source>Fullscreen</source>
        <translation>全屏</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="196"/>
        <source>Exit Fullscreen</source>
        <translation>退出全屏</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="182"/>
        <source>Record</source>
        <translation>录制</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="185"/>
        <source>Stop</source>
        <translation>停止</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="188"/>
        <source>Saving...</source>
        <translation>正在保存…</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="165"/>
        <location filename="../src/app/ToolbarWidget.cpp" line="211"/>
        <source>Volume</source>
        <translation>音量</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="166"/>
        <location filename="../src/app/ToolbarWidget.cpp" line="212"/>
        <source>Pin</source>
        <translation>置顶</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="167"/>
        <location filename="../src/app/ToolbarWidget.cpp" line="213"/>
        <source>Aspect</source>
        <translation>宽高比</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="168"/>
        <location filename="../src/app/ToolbarWidget.cpp" line="214"/>
        <source>Fit</source>
        <translation>适应窗口</translation>
    </message>
    <message>
        <location filename="../src/app/ToolbarWidget.cpp" line="169"/>
        <location filename="../src/app/ToolbarWidget.cpp" line="216"/>
        <source>Settings</source>
        <translation>设置</translation>
    </message>
</context>
</TS>

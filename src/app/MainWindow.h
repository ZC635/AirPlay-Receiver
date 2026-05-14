#pragma once

#include "app/AppSettings.h"
#include "app/SettingsChangeDeferrer.h"

#include <QMainWindow>
#include <QPointer>
#include <QString>

enum class ReceiverState;
class AirPlayReceiver;
class ToolbarWidget;
class HotkeyService;
class QLabel;
class VideoSurfaceWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QString settingsPath, QWidget *parent = nullptr);
    bool isToolbarVisible() const;
    void toggleToolbarVisibility();
    bool isAlwaysOnTopEnabled() const;
    void setAlwaysOnTopEnabled(bool enabled);
    void setVolume(int value);

private:
    void handleShortcut(ShortcutAction action);
    void applyShortcutTooltips();
    bool registerHotkeys();
    bool saveSettings() const;
    void setReceiverVolume(int value);
    void syncVolumeFromReceiver(double volume);
    void handleReceiverNameChange(const QString &receiverName);
    bool applyReceiverNameNow(const QString &receiverName, bool revertOnFailure = true);
    void revertReceiverNameToDefaultAfterApplyFailure();
    void handleVideoQualityChange(const VideoQualitySettings &quality);
    bool applyVideoQualityNow(const VideoQualitySettings &quality);
    void updateReceiverState(ReceiverState state);
    void showSettingsDialog();
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void applyAspectRatioLock(bool enabled);
    void applyVideoFitMode(bool enabled);
    void enforceAspectRatio();

    ToolbarWidget *toolbar_;
    QLabel *statusLabel_;
    VideoSurfaceWidget *videoSurface_;
    AppSettings settings_;
    QString activeReceiverName_;
    SettingsChangeDeferrer deferrer_;
    QPointer<HotkeyService> hotkeys_;
    QPointer<AirPlayReceiver> receiver_;
    QString currentError_;
    QString settingsPath_;
    bool receiverConnected_ = false;
    bool receiverSessionActive_ = false;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    bool aspectRatioLock_ = false;
    bool alwaysOnTopEnabled_ = false;
    bool videoFitMode_ = false;
    VideoQualitySettings activeVideoQuality_;
};

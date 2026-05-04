#pragma once

#include "app/AppSettings.h"

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
    void handleReceiverNameChange(const QString &receiverName);
    bool applyReceiverNameNow(const QString &receiverName);
    void revertReceiverNameToDefaultAfterApplyFailure();
    void applyPendingReceiverNameIfNeeded(bool wasConnected);
    void updateReceiverState(ReceiverState state);
    void showSettingsDialog();

    ToolbarWidget *toolbar_;
    QLabel *statusLabel_;
    VideoSurfaceWidget *videoSurface_;
    AppSettings settings_;
    QString activeReceiverName_;
    QString pendingReceiverName_;
    QPointer<HotkeyService> hotkeys_;
    QPointer<AirPlayReceiver> receiver_;
    QString currentError_;
    QString settingsPath_;
    bool receiverConnected_ = false;
};

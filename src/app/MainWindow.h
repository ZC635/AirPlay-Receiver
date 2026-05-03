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

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QWidget *parent = nullptr);
    bool isToolbarVisible() const;
    void toggleToolbarVisibility();
    bool isAlwaysOnTopEnabled() const;
    void setAlwaysOnTopEnabled(bool enabled);
    void setVolume(int value);

private:
    void handleShortcut(ShortcutAction action);
    void setReceiverVolume(int value);
    void updateReceiverState(ReceiverState state);
    void showSettingsDialog();

    ToolbarWidget *toolbar_;
    QLabel *statusLabel_;
    AppSettings settings_;
    QPointer<AirPlayReceiver> receiver_;
    QString currentError_;
};

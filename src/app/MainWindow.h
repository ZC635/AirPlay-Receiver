#pragma once

#include "app/AppSettings.h"

#include <QMainWindow>

class ToolbarWidget;
class HotkeyService;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent = nullptr);
    bool isToolbarVisible() const;
    void toggleToolbarVisibility();
    bool isAlwaysOnTopEnabled() const;
    void setAlwaysOnTopEnabled(bool enabled);

private:
    void handleShortcut(ShortcutAction action);
    void showSettingsDialog();

    ToolbarWidget *toolbar_;
    AppSettings settings_;
};

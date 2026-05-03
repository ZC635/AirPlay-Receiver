#pragma once

#include "app/AppSettings.h"

#include <QMainWindow>

class ToolbarWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    bool isToolbarVisible() const;
    void toggleToolbarVisibility();
    bool isAlwaysOnTopEnabled() const;
    void setAlwaysOnTopEnabled(bool enabled);

private:
    void showSettingsDialog();

    ToolbarWidget *toolbar_;
    AppSettings settings_;
};

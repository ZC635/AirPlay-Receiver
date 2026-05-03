#include "app/MainWindow.h"

#include "app/SettingsDialog.h"
#include "app/ToolbarWidget.h"
#include "platform/HotkeyService.h"

#include <algorithm>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(AppSettings::defaults(), nullptr, parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent)
    : QMainWindow(parent),
      toolbar_(new ToolbarWidget(this)),
      settings_(std::move(settings)) {
    setWindowTitle("AirPlay Receiver");
    resize(960, 540);

    auto *placeholder = new QWidget(this);
    placeholder->setStyleSheet("background-color: black;");

    auto *layout = new QVBoxLayout(placeholder);
    layout->setAlignment(Qt::AlignTop | Qt::AlignRight);
    layout->addWidget(toolbar_);

    setCentralWidget(placeholder);

    connect(toolbar_, &ToolbarWidget::alwaysOnTopToggled, this, &MainWindow::setAlwaysOnTopEnabled);
    connect(toolbar_, &ToolbarWidget::settingsRequested, this, &MainWindow::showSettingsDialog);

    if (hotkeys != nullptr) {
        for (const auto &binding : settings_.shortcuts()) {
            hotkeys->registerShortcut(binding.action, binding.sequence);
        }
        connect(hotkeys, &HotkeyService::activated, this, &MainWindow::handleShortcut);
    }
}

bool MainWindow::isToolbarVisible() const {
    return !toolbar_->isHidden();
}

void MainWindow::toggleToolbarVisibility() {
    toolbar_->setVisible(!isToolbarVisible());
}

bool MainWindow::isAlwaysOnTopEnabled() const {
    return windowFlags().testFlag(Qt::WindowStaysOnTopHint);
}

void MainWindow::setAlwaysOnTopEnabled(bool enabled) {
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    if (wasVisible) {
        show();
    }
}

void MainWindow::handleShortcut(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::ToggleAlwaysOnTop:
        setAlwaysOnTopEnabled(!isAlwaysOnTopEnabled());
        break;
    case ShortcutAction::VolumeUp:
        toolbar_->setVolume(std::min(toolbar_->volume() + 5, 100));
        break;
    case ShortcutAction::VolumeDown:
        toolbar_->setVolume(std::max(toolbar_->volume() - 5, 0));
        break;
    case ShortcutAction::ToggleToolbar:
        toggleToolbarVisibility();
        break;
    }
}

void MainWindow::showSettingsDialog() {
    SettingsDialog dialog(settings_, this);
    dialog.exec();
}

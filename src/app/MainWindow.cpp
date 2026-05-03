#include "app/MainWindow.h"

#include "app/SettingsDialog.h"
#include "app/ToolbarWidget.h"
#include "backend/AirPlayReceiver.h"
#include "platform/HotkeyService.h"

#include <algorithm>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(AppSettings::defaults(), nullptr, parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent)
    : MainWindow(std::move(settings), hotkeys, nullptr, parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QWidget *parent)
    : QMainWindow(parent),
      toolbar_(new ToolbarWidget(this)),
      statusLabel_(new QLabel("Ready for AirPlay", this)),
      settings_(std::move(settings)),
      receiver_(receiver) {
    setWindowTitle("AirPlay Receiver");
    resize(960, 540);

    auto *placeholder = new QWidget(this);
    placeholder->setStyleSheet("background-color: black;");

    statusLabel_->setObjectName("receiverStatusLabel");
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setStyleSheet("color: white; background: transparent;");

    auto *layout = new QVBoxLayout(placeholder);
    layout->setAlignment(Qt::AlignTop);
    layout->addWidget(toolbar_);
    layout->setAlignment(toolbar_, Qt::AlignTop | Qt::AlignRight);
    layout->addStretch();
    layout->addWidget(statusLabel_);
    layout->setAlignment(statusLabel_, Qt::AlignCenter);
    layout->addStretch();

    setCentralWidget(placeholder);

    connect(toolbar_, &ToolbarWidget::volumeChanged, this, &MainWindow::setReceiverVolume);
    connect(toolbar_, &ToolbarWidget::alwaysOnTopToggled, this, &MainWindow::setAlwaysOnTopEnabled);
    connect(toolbar_, &ToolbarWidget::settingsRequested, this, &MainWindow::showSettingsDialog);

    if (receiver_ != nullptr) {
        updateReceiverState(receiver_->state());
        connect(receiver_, &AirPlayReceiver::stateChanged, this, &MainWindow::updateReceiverState);
        connect(receiver_, &AirPlayReceiver::errorChanged, this, [this](const QString &error) {
            if (error.isEmpty()) {
                updateReceiverState(receiver_->state());
            } else {
                statusLabel_->setText(error);
            }
        });
    }

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
    toolbar_->setAlwaysOnTopChecked(enabled);
    if (wasVisible) {
        show();
    }
}

void MainWindow::setVolume(int value) {
    const int clamped = std::clamp(value, 0, 100);
    if (toolbar_->volume() == clamped) {
        setReceiverVolume(clamped);
        return;
    }

    toolbar_->setVolume(clamped);
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

void MainWindow::setReceiverVolume(int value) {
    if (receiver_ != nullptr) {
        receiver_->setVolume(std::clamp(value, 0, 100) / 100.0);
    }
}

void MainWindow::updateReceiverState(ReceiverState state) {
    switch (state) {
    case ReceiverState::Connecting:
        statusLabel_->setText("Connecting");
        break;
    case ReceiverState::Connected:
        statusLabel_->setText("Connected");
        break;
    case ReceiverState::Error:
        statusLabel_->setText("Error");
        break;
    case ReceiverState::Idle:
    case ReceiverState::Starting:
    case ReceiverState::Discoverable:
        statusLabel_->setText("Ready for AirPlay");
        break;
    }
}

void MainWindow::showSettingsDialog() {
    SettingsDialog dialog(settings_, this);
    dialog.exec();
}

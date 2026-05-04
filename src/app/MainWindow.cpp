#include "app/MainWindow.h"

#include "app/SettingsDialog.h"
#include "app/ToolbarWidget.h"
#include "app/VideoSurfaceWidget.h"
#include "backend/AirPlayReceiver.h"
#include "platform/HotkeyService.h"

#include <algorithm>
#include <QGridLayout>
#include <QKeySequence>
#include <QLabel>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(AppSettings::defaults(), nullptr, parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent)
    : MainWindow(std::move(settings), hotkeys, nullptr, parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QWidget *parent)
    : QMainWindow(parent),
      toolbar_(new ToolbarWidget(this)),
      statusLabel_(new QLabel("Ready for AirPlay", this)),
      videoSurface_(new VideoSurfaceWidget(this)),
      settings_(std::move(settings)),
      receiver_(receiver) {
    setWindowTitle("AirPlay Receiver");
    resize(960, 540);

    statusLabel_->setObjectName("receiverStatusLabel");
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setStyleSheet("color: black; background: transparent;");
    statusLabel_->setAttribute(Qt::WA_NativeWindow, true);
    toolbar_->setAttribute(Qt::WA_NativeWindow, true);

    auto *central = new QWidget(this);
    auto *layout = new QGridLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(videoSurface_, 0, 0);
    layout->addWidget(statusLabel_, 0, 0, Qt::AlignCenter);
    layout->addWidget(toolbar_, 0, 0, Qt::AlignTop | Qt::AlignRight);

    setCentralWidget(central);
    statusLabel_->raise();
    toolbar_->raise();

    const QString volumeUpShortcut = settings_.shortcutFor(ShortcutAction::VolumeUp).toString(QKeySequence::NativeText);
    const QString volumeDownShortcut = settings_.shortcutFor(ShortcutAction::VolumeDown).toString(QKeySequence::NativeText);
    const QString pinShortcut = settings_.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).toString(QKeySequence::NativeText);
    toolbar_->setVolumeShortcutTooltip(QString("Volume: %1 / %2").arg(volumeUpShortcut, volumeDownShortcut));
    toolbar_->setAlwaysOnTopShortcutTooltip(QString("Pin: %1").arg(pinShortcut));

    connect(toolbar_, &ToolbarWidget::volumeChanged, this, &MainWindow::setReceiverVolume);
    connect(toolbar_, &ToolbarWidget::alwaysOnTopToggled, this, &MainWindow::setAlwaysOnTopEnabled);
    connect(toolbar_, &ToolbarWidget::settingsRequested, this, &MainWindow::showSettingsDialog);

    if (receiver_ != nullptr) {
        receiver_->setVideoSurface(videoSurface_->winId());
        updateReceiverState(receiver_->state());
        connect(receiver_, &AirPlayReceiver::stateChanged, this, &MainWindow::updateReceiverState);
        connect(receiver_, &AirPlayReceiver::errorChanged, this, [this](const QString &error) {
            currentError_ = error;
            if (currentError_.isEmpty() && receiver_ != nullptr) {
                updateReceiverState(receiver_->state());
            } else if (currentError_.isEmpty()) {
                statusLabel_->setText("Ready for AirPlay");
            } else {
                statusLabel_->setText(currentError_);
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
    const bool showToolbar = !isToolbarVisible();
    toolbar_->setVisible(showToolbar);
    statusLabel_->setVisible(receiverConnected_ ? showToolbar : true);
    if (!statusLabel_->isHidden()) {
        statusLabel_->raise();
    }
    if (showToolbar) {
        toolbar_->raise();
    }
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
    receiverConnected_ = state == ReceiverState::Connected;
    const bool showToolbar = !receiverConnected_;
    toolbar_->setVisible(showToolbar);
    statusLabel_->setVisible(showToolbar);
    if (showToolbar) {
        statusLabel_->raise();
        toolbar_->raise();
    }

    switch (state) {
    case ReceiverState::Connecting:
        statusLabel_->setText("Connecting");
        break;
    case ReceiverState::Connected:
        statusLabel_->setText("Connected");
        break;
    case ReceiverState::Error:
        statusLabel_->setText(currentError_.isEmpty() ? QString("Ready for AirPlay") : currentError_);
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

#include "app/MainWindow.h"

#include "app/AppSettingsStore.h"
#include "app/SettingsDialog.h"
#include "app/ToolbarWidget.h"
#include "app/VideoSurfaceWidget.h"
#include "backend/AirPlayReceiver.h"
#include "platform/HotkeyService.h"

#include <algorithm>
#include <QGridLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QWidget>
#include <utility>

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(AppSettings::defaults(), nullptr, parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent)
    : MainWindow(std::move(settings), hotkeys, nullptr, QString(), parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QWidget *parent)
    : MainWindow(std::move(settings), hotkeys, receiver, QString(), parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QString settingsPath, QWidget *parent)
    : QMainWindow(parent),
      toolbar_(new ToolbarWidget(this)),
      statusLabel_(new QLabel("Ready for AirPlay", this)),
      videoSurface_(new VideoSurfaceWidget(this)),
      settings_(std::move(settings)),
      activeReceiverName_(settings_.receiverName()),
      hotkeys_(hotkeys),
      receiver_(receiver),
      settingsPath_(std::move(settingsPath)) {
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

    applyShortcutTooltips();

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
        if (receiver_->receiverName() != settings_.receiverName()) {
            receiver_->applyReceiverName(settings_.receiverName());
        }
    }

    setVolume(settings_.volume());

    registerHotkeys();
    if (hotkeys_ != nullptr) {
        connect(hotkeys_, &HotkeyService::activated, this, &MainWindow::handleShortcut);
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

void MainWindow::applyShortcutTooltips() {
    const QString volumeUpShortcut = settings_.shortcutFor(ShortcutAction::VolumeUp).toString(QKeySequence::NativeText);
    const QString volumeDownShortcut = settings_.shortcutFor(ShortcutAction::VolumeDown).toString(QKeySequence::NativeText);
    const QString pinShortcut = settings_.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).toString(QKeySequence::NativeText);
    toolbar_->setVolumeShortcutTooltip(QString("Volume: %1 / %2").arg(volumeUpShortcut, volumeDownShortcut));
    toolbar_->setAlwaysOnTopShortcutTooltip(QString("Pin: %1").arg(pinShortcut));
}

bool MainWindow::registerHotkeys() {
    if (hotkeys_ == nullptr) {
        return true;
    }

    bool registeredAll = true;
    hotkeys_->unregisterAll();
    for (const auto &binding : settings_.shortcuts()) {
        registeredAll = hotkeys_->registerShortcut(binding.action, binding.sequence) && registeredAll;
    }
    return registeredAll;
}

bool MainWindow::saveSettings() const {
    if (settingsPath_.isEmpty()) {
        return true;
    }
    return AppSettingsStore(settingsPath_).save(settings_);
}

void MainWindow::setReceiverVolume(int value) {
    const int clamped = std::clamp(value, 0, 100);
    settings_.setVolume(clamped);
    if (receiver_ != nullptr) {
        receiver_->setVolume(clamped / 100.0);
    }
    if (!saveSettings()) {
        statusLabel_->setText("Could not save settings");
    }
}

void MainWindow::handleReceiverNameChange(const QString &receiverName) {
    if (receiverName == activeReceiverName_) {
        pendingReceiverName_.clear();
        return;
    }

    if (receiver_ == nullptr) {
        activeReceiverName_ = receiverName;
        pendingReceiverName_.clear();
        return;
    }

    if (receiverConnected_) {
        const auto answer = QMessageBox::question(
            this,
            "Apply receiver name",
            "Applying the new receiver name now will disconnect the current iPhone. Apply now?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            pendingReceiverName_ = receiverName;
            return;
        }
    }

    pendingReceiverName_.clear();
    applyReceiverNameNow(receiverName);
}

bool MainWindow::applyReceiverNameNow(const QString &receiverName) {
    if (receiver_ == nullptr) {
        activeReceiverName_ = receiverName;
        return true;
    }

    if (receiver_->applyReceiverName(receiverName)) {
        activeReceiverName_ = receiverName;
        return true;
    }

    revertReceiverNameToDefaultAfterApplyFailure();
    return false;
}

void MainWindow::revertReceiverNameToDefaultAfterApplyFailure() {
    const QString defaultName = AppSettings::defaults().receiverName();
    settings_.setReceiverName(defaultName);
    pendingReceiverName_.clear();
    if (receiver_ != nullptr && receiver_->receiverName() != defaultName) {
        receiver_->applyReceiverName(defaultName);
    }
    activeReceiverName_ = defaultName;

    if (!saveSettings()) {
        statusLabel_->setText("Could not save settings");
        return;
    }
    statusLabel_->setText("Could not apply receiver name; reverted to default");
}

void MainWindow::applyPendingReceiverNameIfNeeded(bool wasConnected) {
    if (!wasConnected || receiverConnected_ || pendingReceiverName_.isEmpty()) {
        return;
    }

    const QString receiverName = pendingReceiverName_;
    pendingReceiverName_.clear();
    applyReceiverNameNow(receiverName);
}

void MainWindow::updateReceiverState(ReceiverState state) {
    const bool wasConnected = receiverConnected_;
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

    applyPendingReceiverNameIfNeeded(wasConnected);
}

void MainWindow::showSettingsDialog() {
    SettingsDialog dialog(settings_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const AppSettings previousSettings = settings_;
    settings_ = dialog.settings();
    if (!registerHotkeys()) {
        settings_ = previousSettings;
        registerHotkeys();
        statusLabel_->setText("Could not register one or more shortcuts");
        return;
    }

    applyShortcutTooltips();
    setVolume(settings_.volume());
    if (!saveSettings()) {
        statusLabel_->setText("Could not save settings");
    }
    const bool receiverNameChanged = settings_.receiverName() != activeReceiverName_;
    if (receiverNameChanged) {
        handleReceiverNameChange(settings_.receiverName());
    }
}

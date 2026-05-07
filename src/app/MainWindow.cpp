#include "app/MainWindow.h"

#include "app/AppSettingsStore.h"
#include "app/SettingsDialog.h"
#include "app/ToolbarWidget.h"
#include "app/VideoSurfaceWidget.h"
#include "backend/AirPlayReceiver.h"
#include "platform/AspectRatioSizing.h"
#include "platform/HotkeyService.h"

#include <algorithm>
#include <cmath>
#include <QGridLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QWidget>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

namespace {
bool setNativeAlwaysOnTop(WId windowId, bool enabled) {
    HWND window = reinterpret_cast<HWND>(windowId);
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }

    return SetWindowPos(
               window,
               enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
               0,
               0,
               0,
               0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
}

bool setWindowBorderColor(WId windowId, bool enabled) {
    HWND window = reinterpret_cast<HWND>(windowId);
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }
    const COLORREF blue = RGB(0x33, 0x96, 0xF3);
    const COLORREF none = 0xFFFFFFFE;
    return SUCCEEDED(DwmSetWindowAttribute(
        window, 34, enabled ? &blue : &none, sizeof(COLORREF)));
}

AspectRatioFrameMargins frameMarginsFor(const QWidget &widget) {
    const QRect frame = widget.frameGeometry();
    const QRect client = widget.geometry();
    const int left = client.left() - frame.left();
    const int top = client.top() - frame.top();
    return AspectRatioFrameMargins{
        left,
        top,
        frame.width() - client.width() - left,
        frame.height() - client.height() - top};
}

AspectRatioSizeConstraints sizeConstraintsFor(const QWidget &widget, const AspectRatioFrameMargins &margins) {
    const int horizontalFrame = std::max(0, margins.left) + std::max(0, margins.right);
    const int verticalFrame = std::max(0, margins.top) + std::max(0, margins.bottom);
    return AspectRatioSizeConstraints{
        widget.minimumWidth() + horizontalFrame,
        widget.minimumHeight() + verticalFrame,
        widget.maximumWidth() + horizontalFrame,
        widget.maximumHeight() + verticalFrame};
}
}

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
    alwaysOnTopEnabled_ = windowFlags().testFlag(Qt::WindowStaysOnTopHint);

    statusLabel_->setObjectName("receiverStatusLabel");
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setStyleSheet("color: black; background: transparent;");
    statusLabel_->setAttribute(Qt::WA_TranslucentBackground, true);

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
    connect(toolbar_, &ToolbarWidget::aspectRatioToggled, this, &MainWindow::applyAspectRatioLock);
    connect(toolbar_, &ToolbarWidget::videoFitToggled, this, &MainWindow::applyVideoFitMode);

    if (receiver_ != nullptr) {
        connect(receiver_, &AirPlayReceiver::videoSizeChanged, this, [this](int width, int height) {
            if (width <= 0 || height <= 0) return;
            videoWidth_ = width;
            videoHeight_ = height;
            if (aspectRatioLock_) {
                enforceAspectRatio();
            }
        });
        receiver_->setVideoFrameCallback([this](QImage frame) {
            videoSurface_->onFrameReady(frame);
        });
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

    applyAspectRatioLock(settings_.aspectRatioLock());
    applyVideoFitMode(settings_.videoFitMode());

    setVolume(settings_.volume());

    const bool hotkeysOk = registerHotkeys();
    if (hotkeys_ != nullptr) {
        connect(hotkeys_, &HotkeyService::activated, this, &MainWindow::handleShortcut);
    }
    if (!hotkeysOk) {
        statusLabel_->setText("Could not register one or more shortcuts");
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
    return alwaysOnTopEnabled_;
}

void MainWindow::setAlwaysOnTopEnabled(bool enabled) {
    if (alwaysOnTopEnabled_ == enabled) {
        toolbar_->setAlwaysOnTopChecked(enabled);
        return;
    }

    const bool wasVisible = isVisible();

    if (wasVisible && setNativeAlwaysOnTop(winId(), enabled)) {
        alwaysOnTopEnabled_ = enabled;
        toolbar_->setAlwaysOnTopChecked(enabled);
        setWindowBorderColor(winId(), enabled);
        return;
    }

    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    alwaysOnTopEnabled_ = enabled;
    toolbar_->setAlwaysOnTopChecked(enabled);
    if (wasVisible) {
        show();
    }
    setWindowBorderColor(winId(), enabled);
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
    case ShortcutAction::ToggleAspectRatio:
        applyAspectRatioLock(!aspectRatioLock_);
        break;
    case ShortcutAction::ToggleVideoFit:
        applyVideoFitMode(!videoFitMode_);
        break;
    }
}

void MainWindow::applyShortcutTooltips() {
    const QString volumeUpShortcut = settings_.shortcutFor(ShortcutAction::VolumeUp).toString(QKeySequence::NativeText);
    const QString volumeDownShortcut = settings_.shortcutFor(ShortcutAction::VolumeDown).toString(QKeySequence::NativeText);
    const QString pinShortcut = settings_.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).toString(QKeySequence::NativeText);
    const QString aspectShortcut = settings_.shortcutFor(ShortcutAction::ToggleAspectRatio).toString(QKeySequence::NativeText);
    const QString videoFitShortcut = settings_.shortcutFor(ShortcutAction::ToggleVideoFit).toString(QKeySequence::NativeText);
    toolbar_->setVolumeShortcutTooltip(QString("Volume: %1 / %2").arg(volumeUpShortcut, volumeDownShortcut));
    toolbar_->setAlwaysOnTopShortcutTooltip(QString("Pin: %1").arg(pinShortcut));
    toolbar_->setAspectRatioShortcutTooltip(QString("Aspect: %1").arg(aspectShortcut));
    toolbar_->setVideoFitShortcutTooltip(QString("Fit: %1").arg(videoFitShortcut));
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
    const bool changed = (settings_.volume() != clamped);
    settings_.setVolume(clamped);
    if (receiver_ != nullptr) {
        receiver_->setVolume(clamped / 100.0);
    }
    if (changed && !saveSettings()) {
        statusLabel_->setText("Could not save settings");
    }
}

void MainWindow::handleReceiverNameChange(const QString &receiverName) {
    if (receiverName == activeReceiverName_) {
        pendingReceiverName_.clear();
        return;
    }

    if (receiverName == pendingReceiverName_) {
        return;
    }

    if (receiver_ == nullptr) {
        activeReceiverName_ = receiverName;
        pendingReceiverName_.clear();
        return;
    }

    if (receiverRenameBlocked_) {
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

bool MainWindow::applyReceiverNameNow(const QString &receiverName, bool revertOnFailure) {
    if (receiver_ == nullptr) {
        activeReceiverName_ = receiverName;
        return true;
    }

    if (receiver_->applyReceiverName(receiverName)) {
        activeReceiverName_ = receiverName;
        return true;
    }

    if (revertOnFailure) {
        revertReceiverNameToDefaultAfterApplyFailure();
    }
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

void MainWindow::applyPendingReceiverNameIfNeeded(bool wasRenameBlocked) {
    if (!wasRenameBlocked || receiverRenameBlocked_ || pendingReceiverName_.isEmpty()) {
        return;
    }

    const QString receiverName = pendingReceiverName_;
    if (applyReceiverNameNow(receiverName, /*revertOnFailure=*/false)) {
        pendingReceiverName_.clear();
    } else if (receiver_ != nullptr) {
        statusLabel_->setText(QString("Could not apply receiver name \"%1\"; will retry")
                                 .arg(receiverName));
    }
}

void MainWindow::updateReceiverState(ReceiverState state) {
    const bool wasRenameBlocked = receiverRenameBlocked_;
    receiverConnected_ = state == ReceiverState::Connected;
    receiverRenameBlocked_ = state == ReceiverState::Connecting || state == ReceiverState::Connected;
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
        if (state == ReceiverState::Discoverable) {
            videoSurface_->reset();
        }
        statusLabel_->setText("Ready for AirPlay");
        break;
    }

    applyPendingReceiverNameIfNeeded(wasRenameBlocked);
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
    handleReceiverNameChange(settings_.receiverName());
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    auto *msg = static_cast<MSG *>(message);
    if (msg != nullptr && msg->message == WM_WINDOWPOSCHANGING) {
        const bool handled = QMainWindow::nativeEvent(eventType, message, result);
        auto *wp = reinterpret_cast<WINDOWPOS *>(msg->lParam);
        RECT currentRect{};
        const HWND hwnd = wp != nullptr && wp->hwnd != nullptr ? wp->hwnd : msg->hwnd;
        if (wp != nullptr && (((wp->flags & SWP_NOMOVE) != 0) || GetWindowRect(hwnd, &currentRect))) {
            updateWindowPosCopyBitsForResize(*wp, currentRect);
        }
        return handled;
    }
    if (msg != nullptr && msg->message == WM_SIZING && aspectRatioLock_ && videoWidth_ > 0 && videoHeight_ > 0) {
        auto *rect = reinterpret_cast<RECT *>(msg->lParam);
        const double targetRatio = static_cast<double>(videoWidth_) / videoHeight_;
        const AspectRatioFrameMargins margins = frameMarginsFor(*this);
        if (rect != nullptr && adjustWindowRectForAspectRatio(*rect, static_cast<unsigned int>(msg->wParam), targetRatio, margins, sizeConstraintsFor(*this, margins))) {
            if (result != nullptr) {
                *result = TRUE;
            }
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if (!aspectRatioLock_ || videoWidth_ <= 0 || videoHeight_ <= 0) return;
    if (resizing_) return;

    QSize newSize = event->size();
    double targetRatio = static_cast<double>(videoWidth_) / videoHeight_;

    int correctedWidth = static_cast<int>(std::lround(newSize.height() * targetRatio));
    if (qAbs(newSize.width() - correctedWidth) <= 1) return;

    if (correctedWidth < minimumWidth()) correctedWidth = minimumWidth();
    resizing_ = true;
    resize(correctedWidth, newSize.height());
    resizing_ = false;
}

void MainWindow::applyAspectRatioLock(bool enabled) {
    const bool changed = (aspectRatioLock_ != enabled);
    aspectRatioLock_ = enabled;
    settings_.setAspectRatioLock(enabled);
    toolbar_->setAspectRatioChecked(enabled);
    if (changed && !saveSettings()) {
        statusLabel_->setText("Could not save settings");
    }
    if (enabled && videoWidth_ > 0 && videoHeight_ > 0) {
        enforceAspectRatio();
    }
}

void MainWindow::applyVideoFitMode(bool enabled) {
    if (videoFitMode_ == enabled) {
        toolbar_->setVideoFitChecked(enabled);
        if (receiver_ != nullptr) {
            receiver_->setVideoFitMode(enabled);
            videoSurface_->setVideoFitMode(enabled);
        }
        return;
    }

    const bool changed = (settings_.videoFitMode() != enabled);
    videoFitMode_ = enabled;
    settings_.setVideoFitMode(enabled);
    toolbar_->setVideoFitChecked(enabled);
    if (receiver_ != nullptr) {
        receiver_->setVideoFitMode(enabled);
        videoSurface_->setVideoFitMode(enabled);
    }
    if (changed && !saveSettings()) {
        statusLabel_->setText("Could not save settings");
    }
}

void MainWindow::enforceAspectRatio() {
    if (videoWidth_ <= 0 || videoHeight_ <= 0) return;
    double targetRatio = static_cast<double>(videoWidth_) / videoHeight_;
    int newWidth = static_cast<int>(std::lround(height() * targetRatio));
    if (newWidth < minimumWidth()) newWidth = minimumWidth();
    resize(newWidth, height());
}

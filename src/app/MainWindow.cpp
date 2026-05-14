#include "app/MainWindow.h"

#include "app/AppSettingsStore.h"
#include "app/SettingsDialog.h"
#include "app/ToolbarWidget.h"
#include "app/VideoSurfaceWidget.h"
#include "backend/AirPlayReceiver.h"
#include "platform/AspectRatioSizing.h"
#include "platform/HotkeyService.h"

#include <algorithm>
#include <QGridLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QResource>
#include <QSignalBlocker>
#include <QWidget>
#include <cmath>
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

void makeNativeOverlay(QWidget *widget) {
    widget->setAttribute(Qt::WA_NativeWindow, true);
    static_cast<void>(widget->winId());
}

void raiseNativeOverlay(QWidget *widget) {
    widget->raise();
    HWND window = reinterpret_cast<HWND>(widget->winId());
    if (window == nullptr || !IsWindow(window)) {
        return;
    }

    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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

static void initializeAppResources() {
    Q_INIT_RESOURCE(app_resources);
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
    initializeAppResources();
    setWindowIcon(QIcon(":/icons/app-icon.ico"));
    setWindowTitle("AirPlay Receiver");
    resize(960, 540);
    alwaysOnTopEnabled_ = windowFlags().testFlag(Qt::WindowStaysOnTopHint);

    statusLabel_->setObjectName("receiverStatusLabel");
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setStyleSheet("color: black; background: transparent;");
    statusLabel_->setAttribute(Qt::WA_TranslucentBackground, true);
    makeNativeOverlay(statusLabel_);
    makeNativeOverlay(toolbar_);

    auto *central = new QWidget(this);
    auto *layout = new QGridLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(videoSurface_, 0, 0);
    layout->addWidget(statusLabel_, 0, 0, Qt::AlignCenter);
    layout->addWidget(toolbar_, 0, 0, Qt::AlignTop | Qt::AlignRight);

    setCentralWidget(central);
    raiseNativeOverlay(statusLabel_);
    raiseNativeOverlay(toolbar_);

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
        connect(receiver_, &AirPlayReceiver::volumeChanged, this, &MainWindow::syncVolumeFromReceiver);
        if (receiver_->receiverName() != settings_.receiverName()) {
            receiver_->applyReceiverName(settings_.receiverName());
        }
    }

    applyAspectRatioLock(settings_.aspectRatioLock());
    applyVideoFitMode(settings_.videoFitMode());

    activeVideoQuality_ = settings_.videoQuality();
    if (receiver_ != nullptr) {
        receiver_->applyVideoQuality(activeVideoQuality_);
    }

    setVolume(settings_.volume());

    const bool hotkeysOk = registerHotkeys();
    if (hotkeys_ != nullptr) {
        connect(hotkeys_, &HotkeyService::activated, this, &MainWindow::handleShortcut);
    }
    if (!hotkeysOk) {
        statusLabel_->setText("Could not register one or more shortcuts");
    }

    connect(&deferrer_, &SettingsChangeDeferrer::receiverNameReady, this, [this](const QString &name) {
        if (applyReceiverNameNow(name, false)) {
            deferrer_.markReceiverNameApplied(name);
        } else if (receiver_ != nullptr) {
            statusLabel_->setText(QString("Could not apply receiver name \"%1\"; will retry")
                                      .arg(name));
        }
    });

    connect(&deferrer_, &SettingsChangeDeferrer::videoQualityReady, this, [this](const VideoQualitySettings &quality) {
        if (applyVideoQualityNow(quality)) {
            deferrer_.markVideoQualityApplied(quality);
        } else if (receiver_ != nullptr) {
            statusLabel_->setText("Could not apply video quality; will retry");
        }
    });
}

bool MainWindow::isToolbarVisible() const {
    return !toolbar_->isHidden();
}

void MainWindow::toggleToolbarVisibility() {
    const bool showToolbar = !isToolbarVisible();
    toolbar_->setVisible(showToolbar);
    statusLabel_->setVisible(!receiverConnected_);
    if (!statusLabel_->isHidden()) {
        raiseNativeOverlay(statusLabel_);
    }
    if (showToolbar) {
        raiseNativeOverlay(toolbar_);
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

void MainWindow::syncVolumeFromReceiver(double volume) {
    const int clamped = std::clamp(static_cast<int>(std::lround(volume * 100.0)), 0, 100);
    const bool changed = (settings_.volume() != clamped);
    settings_.setVolume(clamped);
    if (toolbar_->volume() != clamped) {
        const QSignalBlocker blocker(toolbar_);
        toolbar_->setVolume(clamped);
    }
    if (changed && !saveSettings()) {
        statusLabel_->setText("Could not save settings");
    }
}

void MainWindow::handleReceiverNameChange(const QString &receiverName) {
    if (receiverName == activeReceiverName_) {
        deferrer_.receiverNameChanged(receiverName, activeReceiverName_);
        return;
    }

    if (deferrer_.isReceiverNamePending(receiverName)) {
        return;
    }

    if (receiver_ == nullptr) {
        activeReceiverName_ = receiverName;
        deferrer_.receiverNameChanged(receiverName, activeReceiverName_);
        return;
    }

    if (receiverSessionActive_) {
        const auto answer = QMessageBox::question(
            this,
            "Apply receiver name",
            "Applying the new receiver name now will disconnect the connected device. Apply now?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            deferrer_.receiverNameChanged(receiverName, activeReceiverName_);
            return;
        }
    }

    if (applyReceiverNameNow(receiverName)) {
        deferrer_.markReceiverNameApplied(receiverName);
    }
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
    activeReceiverName_ = defaultName;
    deferrer_.receiverNameChanged(defaultName, activeReceiverName_);
    if (receiver_ != nullptr && receiver_->receiverName() != defaultName) {
        receiver_->applyReceiverName(defaultName);
    }

    if (!saveSettings()) {
        statusLabel_->setText("Could not save settings");
        return;
    }
    statusLabel_->setText("Could not apply receiver name; reverted to default");
}

void MainWindow::handleVideoQualityChange(const VideoQualitySettings &quality) {
    if (quality == activeVideoQuality_) {
        deferrer_.videoQualityChanged(quality, activeVideoQuality_);
        return;
    }

    if (deferrer_.isVideoQualityPending(quality)) {
        return;
    }

    if (receiver_ == nullptr) {
        activeVideoQuality_ = quality;
        deferrer_.videoQualityChanged(quality, activeVideoQuality_);
        return;
    }

    if (receiverSessionActive_) {
        const auto answer = QMessageBox::question(
            this,
            "Apply video quality",
            "Applying the new video quality now will restart AirPlay and disconnect the connected device. Apply now?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            deferrer_.videoQualityChanged(quality, activeVideoQuality_);
            return;
        }
    }

    if (applyVideoQualityNow(quality)) {
        deferrer_.markVideoQualityApplied(quality);
    } else {
        deferrer_.videoQualityChanged(quality, activeVideoQuality_);
        statusLabel_->setText("Could not apply video quality; will retry");
    }
}

bool MainWindow::applyVideoQualityNow(const VideoQualitySettings &quality) {
    if (receiver_ != nullptr && !receiver_->applyVideoQuality(quality)) {
        return false;
    }
    activeVideoQuality_ = quality;
    return true;
}

void MainWindow::updateReceiverState(ReceiverState state) {
    const bool wasSessionActive = receiverSessionActive_;
    receiverConnected_ = state == ReceiverState::Connected;
    receiverSessionActive_ = state == ReceiverState::Connecting || state == ReceiverState::Connected;
    const bool showToolbar = !receiverConnected_;
    toolbar_->setVisible(showToolbar);
    statusLabel_->setVisible(showToolbar);
    if (showToolbar) {
        raiseNativeOverlay(statusLabel_);
        raiseNativeOverlay(toolbar_);
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

    deferrer_.receiverSessionChanged(wasSessionActive, receiverSessionActive_);
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
    handleVideoQualityChange(settings_.videoQuality());
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    auto *msg = static_cast<MSG *>(message);
    if (msg != nullptr && msg->message == WM_WINDOWPOSCHANGING) {
        auto *wp = reinterpret_cast<WINDOWPOS *>(msg->lParam);
        if (wp != nullptr) {
            updateWindowPosCopyBitsForResize(*wp);
        }
        return QMainWindow::nativeEvent(eventType, message, result);
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
    const double targetRatio = static_cast<double>(videoWidth_) / videoHeight_;
    const AspectRatioFrameMargins margins = frameMarginsFor(*this);
    const AspectRatioSizeConstraints constraints = sizeConstraintsFor(*this, margins);
    // Resulting height may differ from current when size constraints are active.
    const AspectRatioOuterSize outer = adjustedOuterSizeDrivenByHeight(
        frameGeometry().height(), targetRatio, margins, constraints);
    resize(clientWidthForOuterWidth(outer.width, margins),
           clientHeightForOuterHeight(outer.height, margins));
}

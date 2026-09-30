#include "app/MainWindow.h"

#include "app/AppSettingsStore.h"
#include "app/DiagnosticRestartCoordinator.h"
#include "app/FullscreenRestoreGeometry.h"
#include "app/LanguageManager.h"
#include "app/SettingsApplyCoordinator.h"
#include "app/SettingsDialog.h"
#include "app/SettingsApplyTypes.h"
#include "app/ToolbarWidget.h"
#include "app/ToolbarVisibilityController.h"
#include "app/VideoSurfaceWidget.h"
#include "app/WindowStateStore.h"
#include "backend/AirPlayReceiver.h"
#include "platform/AspectRatioSizing.h"
#include "platform/DiagnosticLogFolderActions.h"
#include "platform/HotkeyService.h"
#include "platform/RecordingPathActions.h"
#include "platform/WindowsWindowBehavior.h"

#include <algorithm>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QResource>
#include <QSignalBlocker>
#include <QShortcut>
#include <QScreen>
#include <QStringList>
#include <QWidget>
#include <cmath>
#include <utility>

namespace {
constexpr double kAirPlayMinimumVolumeDb = -30.0;
constexpr double kAirPlayMaximumVolumeDb = 0.0;
constexpr auto kWindowStateFileName = "airplay-window-state.dat";

double volumeGainFromSliderPercent(int value) {
    const int clamped = std::clamp(value, 0, 100);
    if (clamped == 0) {
        return 0.0;
    }

    const double sliderFraction = clamped / 100.0;
    const double db = kAirPlayMinimumVolumeDb +
        (kAirPlayMaximumVolumeDb - kAirPlayMinimumVolumeDb) * sliderFraction;
    return std::pow(10.0, 0.05 * db);
}

int sliderPercentFromVolumeGain(double volume) {
    if (volume <= 0.0) {
        return 0;
    }

    const double db = std::clamp(20.0 * std::log10(volume), kAirPlayMinimumVolumeDb, kAirPlayMaximumVolumeDb);
    const double sliderFraction = (db - kAirPlayMinimumVolumeDb) /
        (kAirPlayMaximumVolumeDb - kAirPlayMinimumVolumeDb);
    return std::clamp(static_cast<int>(std::lround(sliderFraction * 100.0)), 0, 100);
}

QString windowStatePathForSettingsPath(const QString &settingsPath) {
    if (settingsPath.isEmpty()) {
        return QString();
    }
    return QFileInfo(settingsPath).absoluteDir().filePath(kWindowStateFileName);
}

QString mainWindowText(const char *sourceText) {
    return QCoreApplication::translate("MainWindow", sourceText);
}

class DefaultDiagnosticUiPrompts final : public DiagnosticUiPrompts {
public:
    bool confirmPrivacy(QWidget *parent, const QString &message) override {
        return QMessageBox::question(parent,
                                     mainWindowText(QT_TRANSLATE_NOOP("MainWindow", "Diagnostic Logging")), message,
                                     QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel) == QMessageBox::Yes;
    }

    bool confirmDiscardDraft(QWidget *parent, const QString &message) override {
        return QMessageBox::question(parent,
                                     mainWindowText(QT_TRANSLATE_NOOP("MainWindow", "Diagnostic Logging")), message,
                                     QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel) == QMessageBox::Yes;
    }

    bool confirmDisconnectMirroring(QWidget *parent, const QString &message) override {
        return QMessageBox::question(parent,
                                     mainWindowText(QT_TRANSLATE_NOOP("MainWindow", "Diagnostic Logging")), message,
                                     QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel) == QMessageBox::Yes;
    }

    void showRestartUnavailable(QWidget *parent, const QString &message) override {
        QMessageBox::warning(parent,
                             mainWindowText(QT_TRANSLATE_NOOP("MainWindow", "Diagnostic Restart Unavailable")),
                             message, QMessageBox::Ok);
    }

    void showRestartFailure(QWidget *parent, const QString &message) override {
        QMessageBox::critical(parent,
                              mainWindowText(QT_TRANSLATE_NOOP("MainWindow", "Diagnostic Restart Failed")),
                              message, QMessageBox::Ok);
    }
};

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
    : MainWindow(std::move(settings), hotkeys, receiver, std::move(settingsPath),
                 nullptr, parent) {}

MainWindow::MainWindow(AppSettings settings, HotkeyService *hotkeys,
                       AirPlayReceiver *receiver, QString settingsPath,
                       RecordingPathActions *recordingPathActions, QWidget *parent,
                       MainWindowRuntimeServices runtimeServices)
    : QMainWindow(parent),
      toolbar_(new ToolbarWidget(this)),
      statusLabel_(new QLabel(this)),
      videoSurface_(new VideoSurfaceWidget(this)),
      settings_(std::move(settings)),
      hotkeys_(hotkeys),
      receiver_(receiver),
      languageManager_(runtimeServices.languageManager),
      settingsPath_(std::move(settingsPath)) {
    if (!settingsPath_.isEmpty()) {
        settingsStore_ = std::make_unique<AppSettingsStore>(settingsPath_);
    }
    settingsApplyCoordinator_ = std::make_unique<SettingsApplyCoordinator>(
        hotkeys_.data(), settingsStore_.get(), receiver_.data(), &deferrer_);
    if (recordingPathActions == nullptr) {
        ownedRecordingPathActions_ = std::make_unique<WindowsRecordingPathActions>();
        recordingPathActions_ = ownedRecordingPathActions_.get();
    } else {
        recordingPathActions_ = recordingPathActions;
    }
    if (runtimeServices.recordingPathActions != nullptr) {
        recordingPathActions_ = runtimeServices.recordingPathActions;
        ownedRecordingPathActions_.reset();
    }
    if (runtimeServices.diagnosticLogFolderActions == nullptr) {
        ownedDiagnosticLogFolderActions_ = std::make_unique<DiagnosticLogFolderActions>(
            QCoreApplication::applicationDirPath());
        diagnosticLogFolderActions_ = ownedDiagnosticLogFolderActions_.get();
    } else {
        diagnosticLogFolderActions_ = runtimeServices.diagnosticLogFolderActions;
    }
    if (runtimeServices.diagnosticRestartCoordinator == nullptr) {
        ownedDiagnosticRestartCoordinator_ = std::make_unique<DiagnosticRestartCoordinator>();
        diagnosticRestartCoordinator_ = ownedDiagnosticRestartCoordinator_.get();
    } else {
        diagnosticRestartCoordinator_ = runtimeServices.diagnosticRestartCoordinator;
    }
    if (runtimeServices.diagnosticPrompts == nullptr) {
        ownedDiagnosticPrompts_ = std::make_unique<DefaultDiagnosticUiPrompts>();
        diagnosticPrompts_ = ownedDiagnosticPrompts_.get();
    } else {
        diagnosticPrompts_ = runtimeServices.diagnosticPrompts;
    }
    quitApplication_ = runtimeServices.quitApplication;
    if (!quitApplication_) {
        quitApplication_ = [] { QCoreApplication::quit(); };
    }
    initializeAppResources();
    setWindowIcon(QIcon(":/icons/app-icon.ico"));
    updateWindowTitle();
    resize(960, 540);
    windowStatePath_ = windowStatePathForSettingsPath(settingsPath_);
    alwaysOnTopEnabled_ = windowFlags().testFlag(Qt::WindowStaysOnTopHint);

    statusLabel_->setObjectName("receiverStatusLabel");
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setStyleSheet("color: black; background: transparent;");
    statusLabel_->setAttribute(Qt::WA_TranslucentBackground, true);
    setStatus(StatusKind::Ready);
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
    toolbarVisibility_ = new ToolbarVisibilityController(central, toolbar_, this);
    toolbarVisibility_->setHoverRevealEnabled(settings_.toolbarHoverReveal());
    connect(toolbarVisibility_, &ToolbarVisibilityController::visibilityChanged, this, [this](bool visible) {
        toolbar_->setVisible(visible);
        if (visible) raiseNativeOverlay(toolbar_);
    });
    raiseNativeOverlay(statusLabel_);
    raiseNativeOverlay(toolbar_);

    retranslateUi();

    connect(toolbar_, &ToolbarWidget::volumeChanged, this, &MainWindow::setReceiverVolume);
    connect(toolbar_, &ToolbarWidget::alwaysOnTopToggled, this, &MainWindow::setAlwaysOnTopEnabled);
    connect(toolbar_, &ToolbarWidget::fullscreenToggled, this, &MainWindow::setFullscreenEnabled);
    connect(toolbar_, &ToolbarWidget::settingsRequested, this, &MainWindow::showSettingsDialog);
    connect(toolbar_, &ToolbarWidget::aspectRatioToggled, this, &MainWindow::applyAspectRatioLock);
    connect(toolbar_, &ToolbarWidget::videoFitToggled, this, &MainWindow::applyVideoFitMode);
    connect(toolbar_, &ToolbarWidget::recordingToggledRequested, this, &MainWindow::toggleRecording);

    auto *fullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullscreenShortcut->setContext(Qt::WindowShortcut);
    fullscreenShortcut->setAutoRepeat(false);
    connect(fullscreenShortcut, &QShortcut::activated, this, [this] {
        if (QApplication::activeModalWidget() == nullptr && QApplication::activePopupWidget() == nullptr) {
            setFullscreenEnabled(!isFullScreen());
        }
    });
    auto *exitFullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    exitFullscreenShortcut->setContext(Qt::WindowShortcut);
    exitFullscreenShortcut->setAutoRepeat(false);
    connect(exitFullscreenShortcut, &QShortcut::activated, this, [this] {
        if (isFullScreen() && QApplication::activeModalWidget() == nullptr
            && QApplication::activePopupWidget() == nullptr) {
            setFullscreenEnabled(false);
        }
    });

    if (receiver_ != nullptr) {
        recordingState_ = receiver_->recordingState();
        connect(receiver_, &AirPlayReceiver::videoSizeChanged, this, [this](int width, int height) {
            if (!decodedFrameSizeKnown_) {
                updateAspectVideoSize(width, height);
            }
        });
        receiver_->setVideoFrameCallback([this](QImage frame) {
            updateAspectVideoSizeFromFrame(frame);
            videoSurface_->onFrameReady(frame);
        });
        updateReceiverState(receiver_->state());
        connect(receiver_, &AirPlayReceiver::stateChanged, this, &MainWindow::updateReceiverState);
        connect(receiver_, &AirPlayReceiver::errorChanged, this, [this](const QString &error) {
            statusDetail_ = error;
            if (statusDetail_.isEmpty() && receiver_ != nullptr) {
                updateReceiverState(receiver_->state());
            } else if (statusDetail_.isEmpty()) {
                setStatus(StatusKind::Ready);
            } else {
                setStatus(StatusKind::ReceiverError, statusDetail_);
            }
        });
        connect(receiver_, &AirPlayReceiver::volumeChanged, this, &MainWindow::syncVolumeFromReceiver);
        connect(receiver_, &AirPlayReceiver::recordingAvailabilityChanged, this,
                [this](bool) { updateRecordingUi(); });
        connect(receiver_, &AirPlayReceiver::recordingStateChanged,
                this, &MainWindow::handleRecordingStateChanged);
        connect(receiver_, &AirPlayReceiver::recordingFinished,
                this, &MainWindow::handleRecordingFinished);
        connect(receiver_, &AirPlayReceiver::recordingFailed,
                this, &MainWindow::handleRecordingFailed);
        connect(receiver_, &QObject::destroyed, this, [this]() {
            updateRecordingUi();
        });
        updateRecordingUi();
    } else {
        updateRecordingUi();
    }

    applyAspectRatioLock(settings_.aspectRatioLock());
    applyVideoFitMode(settings_.videoFitMode());

    if (receiver_ != nullptr) {
        ReceiverConfigurationBatchRequest startupConfiguration;
        startupConfiguration.receiverNameChanged = receiver_->receiverName() != settings_.receiverName();
        startupConfiguration.resolutionChanged = receiver_->videoQuality().resolution
            != settings_.videoQuality().resolution;
        startupConfiguration.frameRateChanged = receiver_->videoQuality().frameRate
            != settings_.videoQuality().frameRate;
        startupConfiguration.requestedReceiverName = settings_.receiverName();
        startupConfiguration.rollbackReceiverName = receiver_->receiverName();
        startupConfiguration.requestedVideoQuality = settings_.videoQuality();
        startupConfiguration.rollbackVideoQuality = receiver_->videoQuality();
        if (startupConfiguration.receiverNameChanged || startupConfiguration.resolutionChanged
            || startupConfiguration.frameRateChanged) {
            receiver_->applyConfigurationBatch(startupConfiguration);
        }
    }

    setVolume(settings_.volume());

    const auto hotkeyFailures = registerHotkeys();
    if (hotkeys_ != nullptr) {
        connect(hotkeys_, &HotkeyService::activated, this, &MainWindow::handleShortcut);
    }
    if (!hotkeyFailures.isEmpty()) {
        startupHotkeyFailures_ = hotkeyFailures;
        setStatus(StatusKind::HotkeyRegistrationFailures);
    }

    connect(&deferrer_, &SettingsChangeDeferrer::receiverConfigurationReady, this,
            [this](const ReceiverConfigurationBatchRequest &batch) {
        const SettingsApplyOutcome outcome = settingsApplyCoordinator_->completeDeferredReceiverApply(
            batch, settings_);
        settings_ = outcome.committedSettings;
        toolbarVisibility_->setHoverRevealEnabled(settings_.toolbarHoverReveal());
        applyShortcutTooltips();
        if (!outcome.mayClose) {
            presentDeferredReceiverApplyFailure(outcome);
        }
    });

    restoreWindowState();
    setDiagnosticLoggingActive(runtimeServices.diagnosticLoggingActive);

    if (diagnosticRestartCoordinator_ != nullptr) {
        connect(diagnosticRestartCoordinator_, &DiagnosticRestartCoordinator::childReady, this, [this] {
            if (activeSettingsDialog_ != nullptr) {
                activeSettingsDialog_->reject();
            }
            if (receiver_ != nullptr) {
                receiver_->stop();
            }
            quitApplication_();
        });
        connect(diagnosticRestartCoordinator_, &DiagnosticRestartCoordinator::failed, this,
                [this](const QString &error) {
                    if (diagnosticPrompts_ != nullptr) {
                        diagnosticPrompts_->showRestartFailure(this, error);
                    }
                });
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::retranslateUi() {
    setDiagnosticLoggingActive(diagnosticLoggingActive_);
    refreshStatus();
    applyShortcutTooltips();
    updateRecordingUi();
}

void MainWindow::updateWindowTitle() {
    setWindowTitle(diagnosticLoggingActive_
        ? QStringLiteral("AirPlay Receiver") + tr(" [Diagnostic Logging]")
        : QStringLiteral("AirPlay Receiver"));
}

void MainWindow::setStatus(StatusKind kind, QString detail) {
    statusKind_ = kind;
    statusDetail_ = std::move(detail);
    refreshStatus();
}

void MainWindow::refreshStatus() {
    switch (statusKind_) {
    case StatusKind::Ready:
        statusLabel_->setText(tr("Ready for AirPlay"));
        break;
    case StatusKind::Connecting:
        statusLabel_->setText(tr("Connecting"));
        break;
    case StatusKind::Connected:
        statusLabel_->setText(tr("Connected"));
        break;
    case StatusKind::ReceiverError:
        statusLabel_->setText(statusDetail_.isEmpty()
                                  ? tr("Ready for AirPlay")
                                  : tr("Receiver error: %1").arg(statusDetail_));
        break;
    case StatusKind::SettingsSaveFailed:
        statusLabel_->setText(tr("Could not save settings"));
        break;
    case StatusKind::NoRecordableContent:
        statusLabel_->setText(tr("No recordable mirrored content"));
        break;
    case StatusKind::RecordingStartFailed:
        statusLabel_->setText(statusDetail_.isEmpty()
                                  ? tr("Could not start recording")
                                  : tr("Could not start recording: %1").arg(statusDetail_));
        break;
    case StatusKind::HotkeyRegistrationFailures:
        statusLabel_->setText(formatHotkeyRegistrationFailures(startupHotkeyFailures_));
        break;
    }
}

void MainWindow::setDiagnosticLoggingActive(bool active) {
    diagnosticLoggingActive_ = active;
    updateWindowTitle();
}

void MainWindow::handleDiagnosticWriteFailure(QString error) {
    if (diagnosticLoggingStopped_) {
        return;
    }
    diagnosticLoggingStopped_ = true;
    setDiagnosticLoggingActive(false);
    emit diagnosticLoggingStopped(std::move(error));
}

bool MainWindow::isToolbarVisible() const {
    return toolbarVisibility_->isVisible();
}

void MainWindow::toggleToolbarVisibility() {
    toolbarVisibility_->toggleManually(QCursor::pos());
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

void MainWindow::setFullscreenEnabled(bool enabled) {
    if (enabled == isFullScreen()) {
        const QSignalBlocker blocker(toolbar_);
        toolbar_->setFullscreenChecked(enabled);
        return;
    }

    toolbarVisibility_->preserveVisibilityUntilPointerMoves(QCursor::pos());
    if (enabled) {
        preFullscreenState_ = WindowStateSnapshot{saveGeometry(), saveState()};
        preFullscreenNormalGeometry_ = geometry();
        preFullscreenMaximized_ = isMaximized();
        showFullScreen();
    } else {
        showNormal();
        if (preFullscreenState_.has_value()) {
            restoreGeometry(preFullscreenState_->geometry);
            restoreState(preFullscreenState_->state);
            if (preFullscreenMaximized_) {
                showMaximized();
            } else {
                QList<QRect> availableScreens;
                for (const QScreen *screen : QGuiApplication::screens()) {
                    availableScreens.append(screen->availableGeometry());
                }
                setGeometry(fullscreenRestoreGeometry(
                    preFullscreenNormalGeometry_, geometry(), availableScreens));
            }
            preFullscreenState_.reset();
        }
    }
    const QSignalBlocker blocker(toolbar_);
    toolbar_->setFullscreenChecked(isFullScreen());
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
    case ShortcutAction::ToggleRecording:
        toggleRecording();
        break;
    }
}

void MainWindow::applyShortcutTooltips() {
    const QString volumeUpShortcut = settings_.shortcutFor(ShortcutAction::VolumeUp).toString(QKeySequence::NativeText);
    const QString volumeDownShortcut = settings_.shortcutFor(ShortcutAction::VolumeDown).toString(QKeySequence::NativeText);
    const QString pinShortcut = settings_.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).toString(QKeySequence::NativeText);
    const QString aspectShortcut = settings_.shortcutFor(ShortcutAction::ToggleAspectRatio).toString(QKeySequence::NativeText);
    const QString videoFitShortcut = settings_.shortcutFor(ShortcutAction::ToggleVideoFit).toString(QKeySequence::NativeText);
    const QString recordingShortcut = settings_.shortcutFor(ShortcutAction::ToggleRecording).toString(QKeySequence::NativeText);
    toolbar_->setVolumeShortcutTooltip(tr("Volume: %1 / %2").arg(volumeUpShortcut, volumeDownShortcut));
    toolbar_->setAlwaysOnTopShortcutTooltip(tr("Pin: %1").arg(pinShortcut));
    toolbar_->setAspectRatioShortcutTooltip(tr("Aspect: %1").arg(aspectShortcut));
    toolbar_->setVideoFitShortcutTooltip(tr("Fit: %1").arg(videoFitShortcut));
    toolbar_->setRecordingShortcutTooltip(tr("Record: %1").arg(recordingShortcut));
}

QVector<MainWindow::HotkeyRegistrationFailure> MainWindow::registerHotkeys() {
    QVector<HotkeyRegistrationFailure> failures;
    if (hotkeys_ == nullptr) {
        return failures;
    }

    hotkeys_->unregisterAll();
    for (const auto &binding : settings_.shortcuts()) {
        const HotkeyRegistrationResult result = hotkeys_->registerShortcut(binding.action, binding.sequence);
        if (!result.registered) {
            failures.append({binding.action, binding.sequence, result});
        }
    }
    return failures;
}

QString MainWindow::formatHotkeyRegistrationFailures(const QVector<HotkeyRegistrationFailure> &failures) const {
    QStringList details;
    for (const auto &failure : failures) {
        const HotkeyError error = failure.result.error.value_or(
            HotkeyError{std::nullopt, tr("Unknown error.")});
        QString reason = error.message;
        if (error.nativeCode.has_value()) {
            reason += QStringLiteral(" [%1]").arg(*error.nativeCode);
        }
        details.append(QStringLiteral("%1 (%2)")
                           .arg(settingsFieldDisplayName(SettingsFieldId::shortcut(failure.action)), reason));
    }
    return tr("Could not register shortcuts: %1").arg(details.join(QStringLiteral("; ")));
}

bool MainWindow::saveSettings() const {
    if (settingsStore_ == nullptr) {
        return true;
    }
    return settingsStore_->save(settings_).success;
}

void MainWindow::restoreWindowState() {
    if (windowStatePath_.isEmpty()) {
        return;
    }

    const std::optional<WindowStateSnapshot> snapshot = WindowStateStore(windowStatePath_).load();
    if (!snapshot.has_value()) {
        return;
    }

    restoreGeometry(snapshot->geometry);
    setWindowState(windowState() & ~Qt::WindowFullScreen);
    restoreState(snapshot->state);
}

bool MainWindow::saveWindowState() const {
    if (windowStatePath_.isEmpty()) {
        return true;
    }
    if (isFullScreen() && preFullscreenState_.has_value()) {
        return WindowStateStore(windowStatePath_).save(*preFullscreenState_);
    }
    return WindowStateStore(windowStatePath_).save({saveGeometry(), saveState()});
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!confirmDiscardRecordingOnExit()) {
        event->ignore();
        return;
    }
    saveWindowState();
    QMainWindow::closeEvent(event);
}

void MainWindow::setReceiverVolume(int value) {
    const int clamped = std::clamp(value, 0, 100);
    const bool changed = (settings_.volume() != clamped);
    settings_.setVolume(clamped);
    if (receiver_ != nullptr) {
        receiver_->setVolume(volumeGainFromSliderPercent(clamped));
    }
    if (changed && !saveSettings()) {
        setStatus(StatusKind::SettingsSaveFailed);
    }
}

void MainWindow::syncVolumeFromReceiver(double volume) {
    const int clamped = sliderPercentFromVolumeGain(volume);
    const bool changed = (settings_.volume() != clamped);
    settings_.setVolume(clamped);
    if (toolbar_->volume() != clamped) {
        const QSignalBlocker blocker(toolbar_);
        toolbar_->setVolume(clamped);
    }
    if (changed && !saveSettings()) {
        setStatus(StatusKind::SettingsSaveFailed);
    }
}

void MainWindow::updateReceiverState(ReceiverState state) {
    const bool wasSessionActive = receiverSessionActive_;
    receiverState_ = state;
    receiverConnected_ = state == ReceiverState::Connected;
    receiverSessionActive_ = state == ReceiverState::Connecting || state == ReceiverState::Connected;
    if (wasSessionActive && !receiverSessionActive_ && isFullScreen()) {
        setFullscreenEnabled(false);
    }
    toolbarVisibility_->receiverStateChanged(state);
    statusLabel_->setVisible(!receiverConnected_);
    if (!receiverConnected_) {
        raiseNativeOverlay(statusLabel_);
    }

    switch (state) {
    case ReceiverState::Connecting:
        setStatus(StatusKind::Connecting);
        break;
    case ReceiverState::Connected:
        setStatus(StatusKind::Connected);
        break;
    case ReceiverState::Error:
        setStatus(statusDetail_.isEmpty() ? StatusKind::Ready : StatusKind::ReceiverError,
                  statusDetail_);
        break;
    case ReceiverState::Idle:
    case ReceiverState::Starting:
    case ReceiverState::Discoverable:
        if (state == ReceiverState::Discoverable || (wasSessionActive && !receiverSessionActive_)) {
            clearDecodedFrameSizeForAspectLock();
            videoSurface_->reset();
        }
        setStatus(StatusKind::Ready);
        break;
    }

    deferrer_.receiverSessionChanged(wasSessionActive, receiverSessionActive_);
}

void MainWindow::showSettingsDialog() {
    SettingsDialog dialog(settings_, this, recordingPathActions_);
    activeSettingsDialog_ = &dialog;
    if (auto *restartButton = dialog.findChild<QPushButton *>("restartWithDiagnosticLoggingButton")) {
        restartButton->setEnabled(!diagnosticLoggingActive_);
    }
    connect(&dialog, &SettingsDialog::applyRequested, this, [this, &dialog](const AppSettings &draft) {
        const QString previousLanguage = settings_.language();
        const RecordingState recordingState = receiver_ == nullptr
            ? RecordingState::Idle : receiver_->recordingState();
        const SettingsApplyPlan plan = settingsApplyCoordinator_->plan(
            settings_, draft, receiverSessionActive_, recordingState);
        const std::optional<ReceiverApplyTiming> timing = chooseReceiverApplyTiming(plan);
        if (!timing.has_value()) {
            return;
        }
        const SettingsApplyOutcome outcome = settingsApplyCoordinator_->execute(plan, *timing);
        settings_ = outcome.committedSettings;
        toolbarVisibility_->setHoverRevealEnabled(settings_.toolbarHoverReveal());
        if (!outcome.globalResult.has_value() && settings_.language() != previousLanguage
            && languageManager_ != nullptr) {
            languageManager_->apply(settings_.language());
            QEvent languageChange(QEvent::LanguageChange);
            QCoreApplication::sendEvent(&dialog, &languageChange);
            retranslateUi();
        }
        applyShortcutTooltips();
        dialog.presentApplyOutcome(outcome);
    });
    connect(&dialog, &SettingsDialog::restartWithDiagnosticLoggingRequested, this,
            [this, &dialog] { restartWithDiagnosticLogging(dialog); });
    connect(&dialog, &SettingsDialog::openDiagnosticLogFolderRequested, this,
            [this, &dialog] { openDiagnosticLogFolder(dialog); });
    dialog.exec();
    activeSettingsDialog_.clear();
}

void MainWindow::restartWithDiagnosticLogging(SettingsDialog &dialog) {
    const RecordingState recordingState = receiver_ == nullptr
        ? recordingState_ : receiver_->recordingState();
    if (recordingState == RecordingState::Recording || recordingState == RecordingState::Finalizing) {
        if (diagnosticPrompts_ != nullptr) {
            diagnosticPrompts_->showRestartUnavailable(
                this, tr("Diagnostic restart is unavailable while a recording is active or being finalized. Finish or discard the recording, then try again."));
        }
        return;
    }
    if (diagnosticPrompts_ == nullptr || !diagnosticPrompts_->confirmPrivacy(
            this, tr("Diagnostic logging records application, Windows, and privacy-filtered network information. Logs stay on this computer and are never uploaded automatically."))) {
        return;
    }
    if (dialog.hasUnappliedChanges()) {
        if (!diagnosticPrompts_->confirmDiscardDraft(
                this, tr("Settings has unapplied changes. Restarting will discard them. Continue?"))) {
            return;
        }
    }
    if (receiverSessionActive_ && (diagnosticPrompts_ == nullptr || !diagnosticPrompts_->confirmDisconnectMirroring(
            this, tr("Restarting disconnects the current mirroring session. Continue?")))) {
        return;
    }
    if (diagnosticRestartCoordinator_ != nullptr) {
        diagnosticRestartCoordinator_->begin(QCoreApplication::applicationFilePath(),
                                             QCoreApplication::applicationPid());
    }
}

void MainWindow::openDiagnosticLogFolder(SettingsDialog &dialog) {
    if (diagnosticLogFolderActions_ == nullptr) {
        return;
    }
    dialog.presentDiagnosticActionError(diagnosticLogFolderActions_->ensureAndOpen());
}

std::optional<ReceiverApplyTiming> MainWindow::chooseReceiverApplyTiming(
    const SettingsApplyPlan &plan) {
    if (!plan.requiresReceiverTimingDecision) {
        return ReceiverApplyTiming::Immediate;
    }

    QMessageBox prompt(QMessageBox::Question, tr("Apply receiver configuration"),
                       tr("Applying receiver configuration now will disconnect the connected device."),
                       QMessageBox::NoButton, this);
    auto *applyNow = prompt.addButton(tr("Disconnect and apply now"), QMessageBox::AcceptRole);
    auto *afterDisconnect = prompt.addButton(tr("Apply after disconnect"), QMessageBox::ActionRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.exec();
    if (prompt.clickedButton() == applyNow) {
        return ReceiverApplyTiming::Immediate;
    }
    if (prompt.clickedButton() == afterDisconnect) {
        return ReceiverApplyTiming::AfterDisconnect;
    }
    return std::nullopt;
}

void MainWindow::presentDeferredReceiverApplyFailure(const SettingsApplyOutcome &outcome) {
    QStringList lines;
    for (const SettingsFieldResult &field : outcome.fieldResults) {
        if (!isFailureStatus(field.status)) {
            continue;
        }
        const QString reason = field.userReason.isEmpty() ? field.reason : field.userReason.render();
        const QString recoveryError = field.userRecoveryError.isEmpty()
            ? field.recoveryError : field.userRecoveryError.render();
        QString line = tr("%1: attempted %2. %3")
                           .arg(settingsFieldDisplayName(field.field),
                                formatSettingsFieldValue(field.attemptedValue), reason);
        if (field.status == SettingsFieldStatus::ApplyFailedRolledBack) {
            line += tr(" Rollback succeeded.");
        } else if (!field.userRecoveryError.isEmpty()) {
            line += QStringLiteral(" ") + recoveryError;
        } else if (!recoveryError.isEmpty()) {
            line += tr(" Recovery failed: %1").arg(recoveryError);
        } else if (field.status == SettingsFieldStatus::RecoveryFailed) {
            line += tr(" Recovery could not be confirmed.");
        }
        lines.append(line);
    }
    if (!lines.isEmpty()) {
        QMessageBox::critical(this, tr("Deferred receiver configuration failed"), lines.join("\n\n"),
                              QMessageBox::Ok);
    }
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    auto *msg = static_cast<MSG *>(message);
    if (msg != nullptr) {
        const WindowsNativeEventResult nativeResult = handleNativeWindowBehaviorEvent(
            msg->message, msg->wParam, msg->lParam, result, this, aspectRatioLock_ && !isFullScreen(), videoWidth_, videoHeight_, videoSurface_);
        if (nativeResult.handled) {
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::applyAspectRatioLock(bool enabled) {
    const bool changed = (settings_.aspectRatioLock() != enabled);
    aspectRatioLock_ = enabled;
    settings_.setAspectRatioLock(enabled);
    toolbar_->setAspectRatioChecked(enabled);
    if (changed && !saveSettings()) {
        setStatus(StatusKind::SettingsSaveFailed);
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
        setStatus(StatusKind::SettingsSaveFailed);
    }
}

void MainWindow::toggleRecording() {
    QPointer<AirPlayReceiver> receiver = receiver_;
    if (receiver == nullptr) {
        setStatus(StatusKind::NoRecordableContent);
        updateRecordingUi();
        return;
    }

    switch (receiver->recordingState()) {
    case RecordingState::Idle: {
        if (!receiver->recordingAvailable()) {
            setStatus(StatusKind::NoRecordableContent);
            updateRecordingUi();
            return;
        }

        const RecordingOptions options{
            settings_.recordingOutputDirectory(),
            settings_.recordingFormat()};
        const RecordingStartResult result = receiver->startRecording(options);
        if (result.accepted) {
            activeRecordingShowCompletionMessage_ =
                settings_.showRecordingCompletionMessage();
            activeRecordingSession_ = true;
            suppressRecordingCompletion_ = false;
        } else {
            setStatus(StatusKind::RecordingStartFailed, result.error);
        }
        updateRecordingUi();
        return;
    }
    case RecordingState::Recording:
        receiver->stopRecording();
        updateRecordingUi();
        return;
    case RecordingState::Finalizing:
        return;
    }
}

void MainWindow::updateRecordingUi() {
    const QPointer<AirPlayReceiver> receiver = receiver_;
    toolbar_->setRecordingUi(
        receiver == nullptr ? RecordingState::Idle : receiver->recordingState(),
        receiver != nullptr && receiver->recordingAvailable());
}

void MainWindow::handleRecordingStateChanged(RecordingState state) {
    const RecordingState previous = recordingState_;
    recordingState_ = state;
    if (previous == RecordingState::Finalizing && state == RecordingState::Idle) {
        recordingReturnedIdlePendingResult_ = true;
    }
    updateRecordingUi();
}

void MainWindow::handleRecordingFinished(const RecordingResult &result) {
    if (exitConfirmationActive_) {
        exitPendingRecordingResult_ = result;
        return;
    }
    const bool showCleanCompletion = activeRecordingSession_ &&
                                     activeRecordingShowCompletionMessage_;
    activeRecordingSession_ = false;
    activeRecordingShowCompletionMessage_ = false;
    if (suppressRecordingCompletion_) {
        recordingReturnedIdlePendingResult_ = false;
        return;
    }
    if (receiver_ != nullptr) {
        receiver_->acknowledgeRecordingResult();
    }
    if (!result.warning.isEmpty() || showCleanCompletion) {
        showRecordingCompletion(result);
    }
    if (recordingReturnedIdlePendingResult_) {
        recordingReturnedIdlePendingResult_ = false;
        deferrer_.recordingStateChanged(RecordingState::Finalizing,
                                        RecordingState::Idle);
    }
}

void MainWindow::handleRecordingFailed(const QString &error) {
    if (exitConfirmationActive_) {
        exitPendingRecordingError_ = error;
        return;
    }
    activeRecordingSession_ = false;
    activeRecordingShowCompletionMessage_ = false;
    if (suppressRecordingCompletion_) {
        recordingReturnedIdlePendingResult_ = false;
        return;
    }
    QMessageBox::critical(
        this, tr("Recording failed"),
        error.isEmpty() ? tr("Recording failed for an unknown reason")
                        : tr("Recording failed: %1").arg(error),
        QMessageBox::Ok);
    if (recordingReturnedIdlePendingResult_) {
        recordingReturnedIdlePendingResult_ = false;
        deferrer_.recordingStateChanged(RecordingState::Finalizing,
                                        RecordingState::Idle);
    }
}

void MainWindow::showRecordingCompletion(const RecordingResult &result) {
    const bool hasWarning = !result.warning.isEmpty();
    const QString nativePath = QDir::toNativeSeparators(
        QFileInfo(result.finalPath).absoluteFilePath());
    const QString text = hasWarning
        ? tr("Recording saved to:\n%1\n\n%2").arg(nativePath, result.warning)
        : tr("Recording saved to:\n%1").arg(nativePath);
    QMessageBox box(hasWarning ? QMessageBox::Warning : QMessageBox::Information,
                    hasWarning ? tr("Recording saved with warning") : tr("Recording saved"),
                    text, QMessageBox::Ok, this);
    auto *openFolder = box.addButton(tr("Open Folder"), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() != openFolder || recordingPathActions_ == nullptr) {
        return;
    }
    const QString error = recordingPathActions_->revealFile(result.finalPath);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Could not open recording"),
                             tr("Could not open recording: %1").arg(error), QMessageBox::Ok);
    }
}

bool MainWindow::confirmDiscardRecordingOnExit() {
    const QPointer<AirPlayReceiver> receiver = receiver_;
    if (receiver == nullptr || receiver->recordingState() == RecordingState::Idle) {
        return true;
    }

    exitConfirmationActive_ = true;
    const auto answer = QMessageBox::warning(
        this, tr("Discard recording?"),
        tr("A recording is still active or being saved. Discard it and exit?"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    exitConfirmationActive_ = false;
    if (answer != QMessageBox::Discard) {
        if (exitPendingRecordingResult_.has_value()) {
            const RecordingResult result = *exitPendingRecordingResult_;
            exitPendingRecordingResult_.reset();
            handleRecordingFinished(result);
        } else if (exitPendingRecordingError_.has_value()) {
            const QString error = *exitPendingRecordingError_;
            exitPendingRecordingError_.reset();
            handleRecordingFailed(error);
        }
        return false;
    }

    suppressRecordingCompletion_ = true;
    exitPendingRecordingResult_.reset();
    exitPendingRecordingError_.reset();
    receiver->discardRecording();
    recordingReturnedIdlePendingResult_ = false;
    activeRecordingSession_ = false;
    activeRecordingShowCompletionMessage_ = false;
    return receiver->recordingState() == RecordingState::Idle;
}

void MainWindow::updateAspectVideoSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    const bool changed = (videoWidth_ != width || videoHeight_ != height);
    videoWidth_ = width;
    videoHeight_ = height;
    if (changed && aspectRatioLock_) {
        enforceAspectRatio();
    }
}

void MainWindow::updateAspectVideoSizeFromFrame(const QImage &frame) {
    if (frame.width() <= 0 || frame.height() <= 0) {
        return;
    }

    decodedFrameSizeKnown_ = true;
    updateAspectVideoSize(frame.width(), frame.height());
}

void MainWindow::clearDecodedFrameSizeForAspectLock() {
    decodedFrameSizeKnown_ = false;
    videoWidth_ = 0;
    videoHeight_ = 0;
}

void MainWindow::enforceAspectRatio() {
    if (isFullScreen()) return;
    if (videoWidth_ <= 0 || videoHeight_ <= 0) return;
    const double targetRatio = static_cast<double>(videoWidth_) / videoHeight_;
    const AspectRatioFrameMargins frameMargins = frameMarginsFor(*this);
    const AspectRatioFrameMargins aspectMargins = isVisible()
        ? aspectTargetMarginsFor(*this, *videoSurface_)
        : frameMargins;
    const AspectRatioSizeConstraints constraints = sizeConstraintsFor(*this, frameMargins);
    // Resulting height may differ from current when size constraints are active.
    const AspectRatioOuterSize outer = adjustedOuterSizeDrivenByHeight(
        frameGeometry().height(), targetRatio, aspectMargins, constraints);
    resize(clientWidthForOuterWidth(outer.width, frameMargins),
           clientHeightForOuterHeight(outer.height, frameMargins));
}

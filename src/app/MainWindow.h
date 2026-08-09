#pragma once

#include "app/AppSettings.h"
#include "app/SettingsChangeDeferrer.h"
#include "platform/HotkeyService.h"

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>

enum class ReceiverState;
class AirPlayReceiver;
class ToolbarWidget;
class QLabel;
class QImage;
class QCloseEvent;
class RecordingPathActions;
class DiagnosticLogFolderActions;
class DiagnosticRestartCoordinator;
class AppSettingsStore;
class SettingsApplyCoordinator;
struct SettingsApplyPlan;
struct SettingsApplyOutcome;
enum class ReceiverApplyTiming;
class VideoSurfaceWidget;
class SettingsDialog;

class DiagnosticUiPrompts {
public:
    virtual ~DiagnosticUiPrompts() = default;
    virtual bool confirmPrivacy(QWidget *, const QString &) = 0;
    virtual bool confirmDiscardDraft(QWidget *, const QString &) = 0;
    virtual bool confirmDisconnectMirroring(QWidget *, const QString &) = 0;
    virtual void showRestartUnavailable(QWidget *, const QString &) = 0;
    virtual void showRestartFailure(QWidget *, const QString &) = 0;
};

struct MainWindowRuntimeServices {
    RecordingPathActions *recordingPathActions = nullptr;
    DiagnosticLogFolderActions *diagnosticLogFolderActions = nullptr;
    DiagnosticRestartCoordinator *diagnosticRestartCoordinator = nullptr;
    DiagnosticUiPrompts *diagnosticPrompts = nullptr;
    bool diagnosticLoggingActive = false;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QString settingsPath, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver,
               QString settingsPath, RecordingPathActions *recordingPathActions,
               QWidget *parent = nullptr,
               MainWindowRuntimeServices runtimeServices = {});
    ~MainWindow() override;
    bool isToolbarVisible() const;
    void toggleToolbarVisibility();
    bool isAlwaysOnTopEnabled() const;
    void setAlwaysOnTopEnabled(bool enabled);
    void setVolume(int value);
    void setDiagnosticLoggingActive(bool active);
    void handleDiagnosticWriteFailure(QString error);

signals:
    void diagnosticLoggingStopped(QString error);

private:
    struct HotkeyRegistrationFailure {
        ShortcutAction action;
        QKeySequence sequence;
        HotkeyRegistrationResult result;
    };

    void handleShortcut(ShortcutAction action);
    void applyShortcutTooltips();
    QVector<HotkeyRegistrationFailure> registerHotkeys();
    static QString formatHotkeyRegistrationFailures(const QVector<HotkeyRegistrationFailure> &failures);
    bool saveSettings() const;
    void restoreWindowState();
    bool saveWindowState() const;
    void closeEvent(QCloseEvent *event) override;
    void setReceiverVolume(int value);
    void syncVolumeFromReceiver(double volume);
    void updateReceiverState(ReceiverState state);
    void showSettingsDialog();
    void restartWithDiagnosticLogging(SettingsDialog &dialog);
    void openDiagnosticLogFolder(SettingsDialog &dialog);
    std::optional<ReceiverApplyTiming> chooseReceiverApplyTiming(const SettingsApplyPlan &plan);
    void presentDeferredReceiverApplyFailure(const SettingsApplyOutcome &outcome);
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void applyAspectRatioLock(bool enabled);
    void applyVideoFitMode(bool enabled);
    void toggleRecording();
    void updateRecordingUi();
    void handleRecordingStateChanged(RecordingState state);
    void handleRecordingFinished(const RecordingResult &result);
    void handleRecordingFailed(const QString &error);
    void showRecordingCompletion(const RecordingResult &result);
    bool confirmDiscardRecordingOnExit();
    void updateAspectVideoSize(int width, int height);
    void updateAspectVideoSizeFromFrame(const QImage &frame);
    void clearDecodedFrameSizeForAspectLock();
    void enforceAspectRatio();

    ToolbarWidget *toolbar_;
    QLabel *statusLabel_;
    VideoSurfaceWidget *videoSurface_;
    AppSettings settings_;
    SettingsChangeDeferrer deferrer_;
    std::unique_ptr<AppSettingsStore> settingsStore_;
    std::unique_ptr<SettingsApplyCoordinator> settingsApplyCoordinator_;
    QPointer<HotkeyService> hotkeys_;
    QPointer<AirPlayReceiver> receiver_;
    std::unique_ptr<RecordingPathActions> ownedRecordingPathActions_;
    RecordingPathActions *recordingPathActions_ = nullptr;
    std::unique_ptr<DiagnosticLogFolderActions> ownedDiagnosticLogFolderActions_;
    DiagnosticLogFolderActions *diagnosticLogFolderActions_ = nullptr;
    std::unique_ptr<DiagnosticRestartCoordinator> ownedDiagnosticRestartCoordinator_;
    DiagnosticRestartCoordinator *diagnosticRestartCoordinator_ = nullptr;
    std::unique_ptr<DiagnosticUiPrompts> ownedDiagnosticPrompts_;
    DiagnosticUiPrompts *diagnosticPrompts_ = nullptr;
    QPointer<SettingsDialog> activeSettingsDialog_;
    QString currentError_;
    QString settingsPath_;
    QString windowStatePath_;
    bool receiverConnected_ = false;
    bool receiverSessionActive_ = false;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    bool decodedFrameSizeKnown_ = false;
    bool aspectRatioLock_ = false;
    bool alwaysOnTopEnabled_ = false;
    bool videoFitMode_ = false;
    RecordingState recordingState_ = RecordingState::Idle;
    bool activeRecordingShowCompletionMessage_ = false;
    bool activeRecordingSession_ = false;
    bool suppressRecordingCompletion_ = false;
    bool recordingReturnedIdlePendingResult_ = false;
    bool exitConfirmationActive_ = false;
    bool diagnosticLoggingActive_ = false;
    bool diagnosticLoggingStopped_ = false;
    std::optional<RecordingResult> exitPendingRecordingResult_;
    std::optional<QString> exitPendingRecordingError_;
};

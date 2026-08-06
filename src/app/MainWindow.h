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
class VideoSurfaceWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver, QString settingsPath, QWidget *parent = nullptr);
    MainWindow(AppSettings settings, HotkeyService *hotkeys, AirPlayReceiver *receiver,
               QString settingsPath, RecordingPathActions *recordingPathActions,
               QWidget *parent = nullptr);
    ~MainWindow() override;
    bool isToolbarVisible() const;
    void toggleToolbarVisibility();
    bool isAlwaysOnTopEnabled() const;
    void setAlwaysOnTopEnabled(bool enabled);
    void setVolume(int value);

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
    void handleReceiverNameChange(const QString &receiverName);
    bool applyReceiverNameNow(const QString &receiverName, bool revertOnFailure = true);
    void revertReceiverNameToDefaultAfterApplyFailure();
    void handleVideoQualityChange(const VideoQualitySettings &quality);
    bool applyVideoQualityNow(const VideoQualitySettings &quality);
    void updateReceiverState(ReceiverState state);
    void showSettingsDialog();
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
    QString activeReceiverName_;
    SettingsChangeDeferrer deferrer_;
    QPointer<HotkeyService> hotkeys_;
    QPointer<AirPlayReceiver> receiver_;
    std::unique_ptr<RecordingPathActions> ownedRecordingPathActions_;
    RecordingPathActions *recordingPathActions_ = nullptr;
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
    VideoQualitySettings activeVideoQuality_;
    RecordingState recordingState_ = RecordingState::Idle;
    bool activeRecordingShowCompletionMessage_ = false;
    bool activeRecordingSession_ = false;
    bool suppressRecordingCompletion_ = false;
    bool recordingReturnedIdlePendingResult_ = false;
    bool exitConfirmationActive_ = false;
    std::optional<RecordingResult> exitPendingRecordingResult_;
    std::optional<QString> exitPendingRecordingError_;
};

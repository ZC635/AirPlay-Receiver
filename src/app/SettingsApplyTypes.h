#pragma once

#include "app/AppSettingsStore.h"
#include "app/UiMessage.h"
#include "backend/ReceiverConfigurationChange.h"

#include <QVector>

#include <optional>
#include <variant>

enum class SettingsFieldKind {
    ReceiverName,
    Language,
    VideoResolution,
    VideoFrameRate,
    Shortcut,
    RecordingFormat,
    RecordingOutputDirectory,
    RecordingCompletionNotification,
    ToolbarHoverReveal,
};

struct SettingsFieldId {
    SettingsFieldKind kind = SettingsFieldKind::ReceiverName;
    std::optional<ShortcutAction> shortcutAction;

    static SettingsFieldId receiverName();
    static SettingsFieldId language();
    static SettingsFieldId videoResolution();
    static SettingsFieldId videoFrameRate();
    static SettingsFieldId shortcut(ShortcutAction action);
    static SettingsFieldId recordingFormat();
    static SettingsFieldId recordingOutputDirectory();
    static SettingsFieldId recordingCompletionNotification();
    static SettingsFieldId toolbarHoverReveal();
};

bool operator==(const SettingsFieldId &left, const SettingsFieldId &right);
bool operator!=(const SettingsFieldId &left, const SettingsFieldId &right);

using SettingsFieldValue = std::variant<QString,
                                        VideoResolution,
                                        VideoFrameRate,
                                        QKeySequence,
                                        RecordingFormat,
                                        bool>;

enum class SettingsFieldStatus {
    Applied,
    Unchanged,
    Deferred,
    ValidationFailed,
    ApplyFailedRolledBack,
    RecoveryFailed,
};

struct SettingsFieldResult {
    SettingsFieldId field;
    SettingsFieldValue attemptedValue;
    SettingsFieldStatus status = SettingsFieldStatus::Unchanged;
    QString reason;
    std::optional<quint32> nativeErrorCode;
    QString recoveryError;
    UiMessage userReason;
    UiMessage userRecoveryError;
};

enum class ReceiverApplyTiming {
    Immediate,
    AfterDisconnect,
};


enum class SettingsApplyGlobalStatus {
    PersistenceFailed,
};

struct SettingsApplyGlobalResult {
    SettingsApplyGlobalStatus status = SettingsApplyGlobalStatus::PersistenceFailed;
    AppSettingsSaveResult persistence;
};

struct SettingsApplyOutcome {
    AppSettings committedSettings;
    QVector<SettingsFieldResult> fieldResults;
    std::optional<SettingsApplyGlobalResult> globalResult;
    bool airPlayDeferred = false;
    bool mayClose = false;
};

QVector<SettingsFieldId> allSettingsFields();
SettingsFieldValue settingsFieldValue(const AppSettings &settings, const SettingsFieldId &field);
void copySettingsField(const AppSettings &source, const SettingsFieldId &field, AppSettings *destination);
QString settingsFieldDisplayName(const SettingsFieldId &field);
QString formatSettingsFieldValue(const SettingsFieldValue &value);
const SettingsFieldResult *resultForField(const QVector<SettingsFieldResult> &results,
                                          const SettingsFieldId &field);
bool isFailureStatus(SettingsFieldStatus status);

enum class SettingsSubmitStatus { Completed, Cancelled, Busy, Interrupted, TimingSelectionRequired };
struct SettingsSubmitResult {
    SettingsSubmitStatus status = SettingsSubmitStatus::Completed;
    std::optional<SettingsApplyOutcome> outcome;
    UiMessage userReason;
    bool settingsSaved = false;
    bool backendInvoked = false;
    std::optional<ReceiverConfigurationBatchResult> backendResult;
};
enum class ReceiverStartPreparationStatus { Prepared, Unavailable, NotIdle, BackendFailure };
struct ReceiverStartPreparationResult {
    ReceiverStartPreparationStatus status = ReceiverStartPreparationStatus::Unavailable;
    std::optional<ReceiverConfigurationBatchResult> backendResult;
    UiMessage userReason;
};
enum class SettingsNotAppliedReason { ReceiverError };
struct SettingsDeferredNotApplied {
    SettingsNotAppliedReason reason = SettingsNotAppliedReason::ReceiverError;
    UiMessage userReason;
};
using SettingsDeferredResult = std::variant<SettingsApplyOutcome, SettingsDeferredNotApplied>;
Q_DECLARE_METATYPE(SettingsDeferredResult)

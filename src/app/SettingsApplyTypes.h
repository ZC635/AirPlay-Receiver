#pragma once

#include "app/AppSettingsStore.h"

#include <QVector>

#include <optional>
#include <variant>

enum class SettingsFieldKind {
    ReceiverName,
    VideoResolution,
    VideoFrameRate,
    Shortcut,
    RecordingFormat,
    RecordingOutputDirectory,
    RecordingCompletionNotification,
};

struct SettingsFieldId {
    SettingsFieldKind kind = SettingsFieldKind::ReceiverName;
    std::optional<ShortcutAction> shortcutAction;

    static SettingsFieldId receiverName();
    static SettingsFieldId videoResolution();
    static SettingsFieldId videoFrameRate();
    static SettingsFieldId shortcut(ShortcutAction action);
    static SettingsFieldId recordingFormat();
    static SettingsFieldId recordingOutputDirectory();
    static SettingsFieldId recordingCompletionNotification();
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
};

enum class ReceiverApplyTiming {
    Immediate,
    AfterDisconnect,
};

struct SettingsApplyPlan {
    AppSettings baseline;
    AppSettings candidate;
    QVector<SettingsFieldResult> validationResults;
    QVector<SettingsFieldId> validChangedReceiverFields;
    bool receiverSessionActive = false;
    bool recordingIdle = false;
    bool requiresReceiverTimingDecision = false;
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

#include "app/SettingsApplyCoordinator.h"
#include "app/SettingsDiagnostics.h"

#include "app/SettingsChangeDeferrer.h"
#include "app/UiMessage.h"
#include "backend/AirPlayReceiver.h"
#include "platform/HotkeyService.h"
#include "platform/WindowsHotkeyService.h"

#include <algorithm>
#include <QHash>

#include <utility>

struct SettingsApplyPlan {
    AppSettings baseline;
    AppSettings candidate;
    QVector<SettingsFieldResult> validationResults;
    QVector<SettingsFieldId> validChangedReceiverFields;
    bool receiverSessionActive = false;
    bool recordingIdle = false;
    bool requiresReceiverTimingDecision = false;
};

namespace {

SettingsFieldResult *mutableResultForField(QVector<SettingsFieldResult> *results,
                                           const SettingsFieldId &field) {
    if (results == nullptr) {
        return nullptr;
    }

    for (SettingsFieldResult &result : *results) {
        if (result.field == field) {
            return &result;
        }
    }
    return nullptr;
}

UiMessage delayedMessage(const char *source, QStringList arguments = {}) {
    return UiMessage::translated(QStringLiteral("SettingsApplyCoordinator"),
                                 QString::fromLatin1(source), std::move(arguments));
}

UiMessage submitFailureReason(const SettingsSubmitResult &result) {
    if (result.status == SettingsSubmitStatus::Interrupted) {
        if (result.outcome && result.outcome->globalResult) {
            const auto &persistence = result.outcome->globalResult->persistence;
            if (persistence.failureStage || persistence.fileError != QFileDevice::NoError
                || !persistence.errorString.isEmpty()) {
                return result.settingsSaved
                    ? delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Settings application was interrupted. Settings were saved, but their final saved state could not be confirmed. Saving failed for %1: %2. Apply again."),
                                     {persistence.targetPath, persistence.errorString})
                    : delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Settings application was interrupted before saving. Settings could not be saved to %1: %2. Apply again."),
                                     {persistence.targetPath, persistence.errorString});
            }
            if (result.settingsSaved)
                return delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Settings application was interrupted. Settings were saved, but their final saved state could not be confirmed. Apply again."));
        }
        return result.settingsSaved
            ? delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Settings application was interrupted. Saved changes are kept."))
            : delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Settings application was interrupted before saving."));
    }
    return delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver state changed. Apply again to choose when to apply receiver settings."));
}
void markValidationFailure(SettingsFieldResult *result, const QString &reason,
                           const char *source, QStringList arguments = {}) {
    if (result == nullptr) {
        return;
    }
    result->status = SettingsFieldStatus::ValidationFailed;
    result->reason = reason;
    result->userReason = delayedMessage(source, std::move(arguments));
}

bool isReceiverField(const SettingsFieldId &field) {
    return field.kind == SettingsFieldKind::ReceiverName
        || field.kind == SettingsFieldKind::VideoResolution
        || field.kind == SettingsFieldKind::VideoFrameRate;
}

bool isShortcutField(const SettingsFieldId &field) {
    return field.kind == SettingsFieldKind::Shortcut && field.shortcutAction.has_value();
}

bool fieldsDiffer(const AppSettings &baseline,
                  const AppSettings &candidate,
                  const SettingsFieldId &field) {
    return settingsFieldValue(baseline, field) != settingsFieldValue(candidate, field);
}

QString describeHotkeyError(const HotkeyError &error) {
    QString description = error.message;
    if (error.nativeCode.has_value()) {
        if (!description.isEmpty()) {
            description += QStringLiteral(" ");
        }
        description += QStringLiteral("(native error %1)").arg(*error.nativeCode);
    }
    return description;
}

QString shortcutDescription(const SettingsFieldResult &result) {
    return QStringLiteral("%1 shortcut %2")
        .arg(settingsFieldDisplayName(result.field), formatSettingsFieldValue(result.attemptedValue));
}

void setHotkeyFailure(SettingsFieldResult *fieldResult, const HotkeyRegistrationResult &registration) {
    if (fieldResult == nullptr) {
        return;
    }

    const QString action = shortcutDescription(*fieldResult);
    const QString directError = registration.error.has_value()
        ? describeHotkeyError(*registration.error)
        : QStringLiteral("The shortcut could not be registered.");
    if (registration.error.has_value()) {
        fieldResult->nativeErrorCode = registration.error->nativeCode;
    }
    fieldResult->reason = QStringLiteral("%1 could not be registered: %2").arg(action, directError);
    fieldResult->userReason = registration.error.has_value()
        ? delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut registration failed: %1"),
                         {registration.error->message})
        : delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut registration failed."));

    if (registration.previousRestored) {
        fieldResult->status = SettingsFieldStatus::ApplyFailedRolledBack;
        fieldResult->reason += QStringLiteral(" The previous shortcut was restored.");
        fieldResult->userReason = registration.error.has_value()
            ? delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut registration failed: %1. Previous shortcut was restored."),
                             {registration.error->message})
            : delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut registration failed. Previous shortcut was restored."));
        return;
    }

    fieldResult->status = SettingsFieldStatus::RecoveryFailed;
    fieldResult->recoveryError = QStringLiteral(
        "The previous shortcut could not be restored; %1 currently has no confirmed global shortcut.")
        .arg(action);
    fieldResult->userRecoveryError = delayedMessage(
        QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut restoration could not be confirmed."));
    if (registration.recoveryError.has_value()) {
        fieldResult->recoveryError += QStringLiteral(" Recovery failed: %1")
            .arg(describeHotkeyError(*registration.recoveryError));
        fieldResult->userRecoveryError = registration.recoveryError->nativeCode.has_value()
            ? delayedMessage(
                  QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut restoration failed: %1 (native error %2)"),
                  {registration.recoveryError->message,
                   QString::number(*registration.recoveryError->nativeCode)})
            : delayedMessage(
                  QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut restoration failed: %1"),
                  {registration.recoveryError->message});
    }
}

QString compensationFailureDescription(const HotkeyRegistrationResult &registration) {
    QStringList details;
    if (registration.error.has_value()) {
        details.append(describeHotkeyError(*registration.error));
    }
    if (registration.recoveryError.has_value()) {
        details.append(describeHotkeyError(*registration.recoveryError));
    }
    return details.isEmpty() ? QString() : QStringLiteral(" Details: %1").arg(details.join("; "));
}

QString rawHotkeyFailureDetails(const HotkeyRegistrationResult &registration) {
    QStringList details;
    if (registration.error.has_value() && !registration.error->message.isEmpty()) {
        details.append(registration.error->message);
    }
    if (registration.recoveryError.has_value() && !registration.recoveryError->message.isEmpty()) {
        details.append(registration.recoveryError->message);
    }
    return details.join(QStringLiteral("; "));
}

bool batchChangesField(const ReceiverConfigurationBatchRequest &batch,
                       const SettingsFieldId &field) {
    switch (field.kind) {
    case SettingsFieldKind::ReceiverName:
        return batch.receiverNameChanged;
    case SettingsFieldKind::VideoResolution:
        return batch.resolutionChanged;
    case SettingsFieldKind::VideoFrameRate:
        return batch.frameRateChanged;
    default:
        return false;
    }
}

bool hasReceiverChanges(const ReceiverConfigurationBatchRequest &batch) {
    return batch.receiverNameChanged || batch.resolutionChanged || batch.frameRateChanged;
}

ReceiverConfigurationBatchRequest receiverBatchForPlan(const SettingsApplyPlan &plan) {
    ReceiverConfigurationBatchRequest batch;
    batch.receiverNameChanged = plan.validChangedReceiverFields.contains(SettingsFieldId::receiverName());
    batch.resolutionChanged = plan.validChangedReceiverFields.contains(SettingsFieldId::videoResolution());
    batch.frameRateChanged = plan.validChangedReceiverFields.contains(SettingsFieldId::videoFrameRate());
    batch.requestedReceiverName = plan.candidate.receiverName();
    batch.rollbackReceiverName = plan.baseline.receiverName();
    batch.requestedVideoQuality = plan.candidate.videoQuality();
    batch.rollbackVideoQuality = plan.baseline.videoQuality();
    return batch;
}

QString describeReceiverConfiguration(const QString &name, const VideoQualitySettings &quality) {
    return QStringLiteral("receiver name '%1', %2, %3")
        .arg(name, formatSettingsFieldValue(SettingsFieldValue{quality.resolution}),
             formatSettingsFieldValue(SettingsFieldValue{quality.frameRate}));
}

QString describeRequestedConfiguration(const ReceiverConfigurationBatchRequest &batch) {
    return describeReceiverConfiguration(batch.requestedReceiverName,
                                         mergedReceiverConfigurationVideoQuality(batch));
}

QString describeRollbackConfiguration(const ReceiverConfigurationBatchRequest &batch) {
    return describeReceiverConfiguration(batch.rollbackReceiverName, batch.rollbackVideoQuality);
}

void restoreBatchFields(const ReceiverConfigurationBatchRequest &batch, AppSettings *settings) {
    if (settings == nullptr) {
        return;
    }
    if (batch.receiverNameChanged) {
        settings->setReceiverName(batch.rollbackReceiverName);
    }
    VideoQualitySettings quality = settings->videoQuality();
    if (batch.resolutionChanged) {
        quality.resolution = batch.rollbackVideoQuality.resolution;
    }
    if (batch.frameRateChanged) {
        quality.frameRate = batch.rollbackVideoQuality.frameRate;
    }
    if (batch.resolutionChanged || batch.frameRateChanged) {
        settings->setVideoQuality(quality);
    }
}

void updateMayClose(SettingsApplyOutcome *outcome) {
    if (outcome == nullptr) {
        return;
    }
    outcome->mayClose = !outcome->globalResult.has_value()
        && std::all_of(outcome->fieldResults.cbegin(), outcome->fieldResults.cend(),
                       [](const SettingsFieldResult &fieldResult) {
            return fieldResult.status == SettingsFieldStatus::Applied
                || fieldResult.status == SettingsFieldStatus::Unchanged
                || fieldResult.status == SettingsFieldStatus::Deferred;
        });
}

void markReceiverFieldsDeferred(SettingsApplyOutcome *outcome,
                                const ReceiverConfigurationBatchRequest &batch) {
    if (outcome == nullptr) {
        return;
    }
    for (SettingsFieldResult &fieldResult : outcome->fieldResults) {
        if (fieldResult.status != SettingsFieldStatus::ValidationFailed
            && batchChangesField(batch, fieldResult.field)) {
            fieldResult.status = SettingsFieldStatus::Deferred;
            fieldResult.reason = QStringLiteral("Receiver configuration will be applied when its blocker clears.");
            fieldResult.userReason = delayedMessage(
                QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver configuration will be applied when its blocker clears."));
        }
    }
    outcome->airPlayDeferred = true;
}

void markReceiverRecoveryFailure(SettingsApplyOutcome *outcome,
                                 const ReceiverConfigurationBatchRequest &batch,
                                 const ReceiverConfigurationBatchResult &result,
                                 const QString &knownSavedState) {
    if (outcome == nullptr) {
        return;
    }
    const QString applyError = result.applyError.isEmpty()
        ? QStringLiteral("The receiver configuration could not be applied.") : result.applyError;
    const QString runtime = describeReceiverConfiguration(result.knownRuntimeReceiverName,
                                                           result.knownRuntimeVideoQuality);
    for (SettingsFieldResult &fieldResult : outcome->fieldResults) {
        if (fieldResult.status == SettingsFieldStatus::ValidationFailed
            || !batchChangesField(batch, fieldResult.field)) {
            continue;
        }
        fieldResult.status = SettingsFieldStatus::RecoveryFailed;
        fieldResult.reason = QStringLiteral("Receiver configuration apply failed: %1. Known saved state is %2.")
            .arg(applyError, knownSavedState);
        fieldResult.userReason = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver configuration could not be applied: %1"),
            {applyError});
        fieldResult.recoveryError = QStringLiteral(
            "Receiver restoration failed: %1. Known runtime state is %2 and cannot be confirmed.")
            .arg(result.recoveryError.isEmpty()
                     ? QStringLiteral("the rollback configuration did not complete")
                     : result.recoveryError,
                 runtime);
        fieldResult.userRecoveryError = result.recoveryError.isEmpty()
            ? delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver restoration could not be confirmed."))
            : delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver restoration failed: %1"),
                             {result.recoveryError});
    }
}

void markReceiverRollback(SettingsApplyOutcome *outcome,
                          const ReceiverConfigurationBatchRequest &batch,
                          const ReceiverConfigurationBatchResult &result) {
    if (outcome == nullptr) {
        return;
    }
    const QString applyError = result.applyError.isEmpty()
        ? QStringLiteral("The receiver configuration could not be applied.") : result.applyError;
    for (SettingsFieldResult &fieldResult : outcome->fieldResults) {
        if (fieldResult.status != SettingsFieldStatus::ValidationFailed
            && batchChangesField(batch, fieldResult.field)) {
            fieldResult.status = SettingsFieldStatus::ApplyFailedRolledBack;
            fieldResult.reason = QStringLiteral(
                "Receiver configuration apply failed: %1. The previous receiver configuration was restored.")
                .arg(applyError);
            fieldResult.userReason = delayedMessage(
                QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver configuration could not be applied: %1. Previous receiver configuration was restored."),
                {applyError});
        }
    }
}

void markReceiverCompensationFailure(SettingsApplyOutcome *outcome,
                                     const ReceiverConfigurationBatchRequest &batch,
                                     const ReceiverConfigurationBatchResult &result,
                                     const AppSettingsSaveResult &compensation) {
    if (outcome == nullptr) {
        return;
    }
    const QString applyError = result.applyError.isEmpty()
        ? QStringLiteral("The receiver configuration could not be applied.") : result.applyError;
    const QString runtime = describeReceiverConfiguration(result.knownRuntimeReceiverName,
                                                           result.knownRuntimeVideoQuality);
    for (SettingsFieldResult &fieldResult : outcome->fieldResults) {
        if (fieldResult.status == SettingsFieldStatus::ValidationFailed
            || !batchChangesField(batch, fieldResult.field)) {
            continue;
        }
        fieldResult.status = SettingsFieldStatus::RecoveryFailed;
        fieldResult.reason = QStringLiteral(
            "Receiver configuration apply failed: %1. Known saved state is %2; the compensating save did not complete.")
            .arg(applyError, describeRequestedConfiguration(batch));
        fieldResult.userReason = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver configuration could not be applied: %1"),
            {applyError});
        fieldResult.recoveryError = QStringLiteral(
            "Receiver runtime rollback completed. Known runtime state is %1. Compensating JSON save failed: %2. "
            "The saved state cannot be confirmed.")
            .arg(runtime, compensation.errorString);
        fieldResult.userRecoveryError = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Compensating settings save failed: %1"),
            {compensation.errorString});
    }
}

void markUnavailableReceiverCompensated(SettingsApplyOutcome *outcome,
                                        const ReceiverConfigurationBatchRequest &batch,
                                        const ReceiverConfigurationBatchResult &unavailable) {
    if (outcome == nullptr) {
        return;
    }
    const QString runtime = describeReceiverConfiguration(unavailable.knownRuntimeReceiverName,
                                                           unavailable.knownRuntimeVideoQuality);
    const bool runtimeUnavailable = unavailable.knownRuntimeReceiverName == QStringLiteral("unavailable");
    for (SettingsFieldResult &fieldResult : outcome->fieldResults) {
        if (fieldResult.status == SettingsFieldStatus::ValidationFailed
            || !batchChangesField(batch, fieldResult.field)) {
            continue;
        }
        fieldResult.status = SettingsFieldStatus::RecoveryFailed;
        fieldResult.reason = QStringLiteral(
            "Receiver configuration did not start: %1. The saved state was restored to %2.")
            .arg(unavailable.applyError, describeRollbackConfiguration(batch));
        fieldResult.userReason = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver configuration did not start. The saved state was restored."));
        fieldResult.recoveryError = runtimeUnavailable
            ? QStringLiteral("The receiver is unavailable; runtime state is unconfirmed.")
            : QStringLiteral("No receiver operation was scheduled. Known runtime state is %1 and is unchanged.")
                  .arg(runtime);
        fieldResult.userRecoveryError = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver runtime state could not be confirmed."));
    }
}

void markUnavailableReceiverCompensationFailure(
    SettingsApplyOutcome *outcome,
    const ReceiverConfigurationBatchRequest &batch,
    const ReceiverConfigurationBatchResult &unavailable,
    const AppSettingsSaveResult &compensation) {
    if (outcome == nullptr) {
        return;
    }
    const QString runtime = describeReceiverConfiguration(unavailable.knownRuntimeReceiverName,
                                                           unavailable.knownRuntimeVideoQuality);
    const bool runtimeUnavailable = unavailable.knownRuntimeReceiverName == QStringLiteral("unavailable");
    for (SettingsFieldResult &fieldResult : outcome->fieldResults) {
        if (fieldResult.status == SettingsFieldStatus::ValidationFailed
            || !batchChangesField(batch, fieldResult.field)) {
            continue;
        }
        fieldResult.status = SettingsFieldStatus::RecoveryFailed;
        fieldResult.reason = QStringLiteral(
            "Receiver configuration did not start: %1. The requested JSON was saved, but restoring it failed.")
            .arg(unavailable.applyError);
        fieldResult.userReason = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver configuration did not start. The requested settings were saved, but restoring them failed."));
        fieldResult.recoveryError = QStringLiteral(
            "Compensating JSON save failed: %1. The saved state cannot be confirmed. Known runtime state is %2%3.")
            .arg(compensation.errorString, runtime,
                 runtimeUnavailable ? QStringLiteral(" (receiver unavailable)")
                                  : QStringLiteral(" (unchanged because no receiver operation was scheduled)"));
        fieldResult.userRecoveryError = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Compensating settings save failed: %1"),
            {compensation.errorString});
    }
}

// A synchronous dependency can commit independent preferences on either side of
// its own durable write. Reconcile that delta once; a write-only port cannot
// establish the final ordering merely by adopting the newer in-memory values.
bool mergeIndependentCommits(AppSettings *target, const AppSettings &before,
                             const AppSettings &after) {
    bool changed = false;
    for (const auto &field : allSettingsFields()) {
        if (!isReceiverField(field) && fieldsDiffer(before, after, field)
            && fieldsDiffer(*target, after, field)) {
            copySettingsField(after, field, target);
            changed = true;
        }
    }
    if (before.volume() != after.volume() && target->volume() != after.volume()) {
        target->setVolume(after.volume()); changed = true;
    }
    if (before.aspectRatioLock() != after.aspectRatioLock()
        && target->aspectRatioLock() != after.aspectRatioLock()) {
        target->setAspectRatioLock(after.aspectRatioLock()); changed = true;
    }
    if (before.videoFitMode() != after.videoFitMode() && target->videoFitMode() != after.videoFitMode()) {
        target->setVideoFitMode(after.videoFitMode()); changed = true;
    }
    return changed;
}

struct SaveAdoption {
    AppSettingsSaveResult result;
    bool firstWriteSucceeded = false;
};

SaveAdoption saveAndAdopt(AppSettings &current, const AppSettings &requested,
                         SettingsPersistence *persistence, DiagnosticLogSink *sink,
                         const char *origin, const std::function<bool()> &stillCurrent) {
    const AppSettings before = current;
    SaveAdoption saved{SettingsDiagnostics::save(persistence, requested, sink, origin)};
    if (!saved.result.success) return saved;
    saved.firstWriteSucceeded = true;
    AppSettings merged = requested;
    const bool changed = mergeIndependentCommits(&merged, before, current);
    current = merged;
    if (!changed) return saved;
    if (stillCurrent()) {
        const AppSettings beforeReconciliation = current;
        saved.result = SettingsDiagnostics::save(persistence, merged, sink, origin);
        const bool changedAgain = mergeIndependentCommits(&merged, beforeReconciliation, current);
        current = merged;
        if (!saved.result.success || !changedAgain) return saved;
    }
    // Both native saves may have succeeded. This is an unconfirmed ordering,
    // never an invented native Open/Write/Commit error and never a retry loop.
    saved.result = AppSettingsSaveResult{};
    return saved;
}

void markUnconfirmedSavedState(SettingsApplyOutcome *outcome,
                              const ReceiverConfigurationBatchRequest &batch,
                              const ReceiverConfigurationBatchResult *backend = nullptr) {
    for (auto &field : outcome->fieldResults) {
        if (field.status == SettingsFieldStatus::ValidationFailed || !batchChangesField(batch, field.field)) continue;
        field.status = SettingsFieldStatus::RecoveryFailed;
        field.reason = QStringLiteral("A save completed, but the final saved state could not be confirmed.");
        field.userReason = delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator",
            "Settings were saved, but their final saved state could not be confirmed. Apply again."));
        if (backend != nullptr) {
            field.recoveryError = QStringLiteral("Known receiver runtime configuration is %1. Apply error: %2. Recovery error: %3.")
                .arg(describeReceiverConfiguration(backend->knownRuntimeReceiverName, backend->knownRuntimeVideoQuality),
                     backend->applyError, backend->recoveryError);
            field.userRecoveryError = delayedMessage(
                QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver configuration could not be applied: %1"),
                {backend->applyError});
        }
    }
}


void compensateUnavailableReceiver(SettingsApplyOutcome *outcome,
                                   const ReceiverConfigurationBatchRequest &batch,
                                   AppSettings &current,
                                   SettingsPersistence *persistence,
                                   const ReceiverConfigurationBatchResult &unavailable,
                                   DiagnosticLogSink *diagnosticSink,
                                   const std::function<bool()> &stillCurrent) {
    if (outcome == nullptr) {
        return;
    }
    AppSettings compensated = current;
    restoreBatchFields(batch, &compensated);
    const auto saved = saveAndAdopt(current, compensated, persistence, diagnosticSink, "compensation", stillCurrent);
    const auto &compensation = saved.result;
    outcome->committedSettings = current;
    if (compensation.success) {
        markUnavailableReceiverCompensated(outcome, batch, unavailable);
        return;
    }

    outcome->globalResult = SettingsApplyGlobalResult{
        SettingsApplyGlobalStatus::PersistenceFailed, compensation};
    if (saved.firstWriteSucceeded) markUnconfirmedSavedState(outcome, batch, &unavailable);
    else markUnavailableReceiverCompensationFailure(outcome, batch, unavailable, compensation);
}

SettingsApplyOutcome makeDeferredCompletionOutcome(const AppSettings &currentlyCommitted) {
    SettingsApplyOutcome outcome;
    outcome.committedSettings = currentlyCommitted;
    for (const SettingsFieldId &field : allSettingsFields()) {
        outcome.fieldResults.append({field, settingsFieldValue(currentlyCommitted, field)});
    }
    return outcome;
}

} // namespace

SettingsApplyCoordinator::SettingsApplyCoordinator(AppSettings &current, HotkeyService *hotkeys,
    SettingsPersistence *persistence, AirPlayReceiver *receiver,
    DiagnosticLogSink *diagnosticSink, QObject *parent, RecordingPresentationDispatcher presentationDispatcher)
    : QObject(parent), hotkeys_(hotkeys), persistence_(persistence), receiver_(receiver),
      deferrer_(std::make_unique<SettingsChangeDeferrer>()), current_(current), presentationDispatcher_(std::move(presentationDispatcher)), diagnosticSink_(diagnosticSink) {
    qRegisterMetaType<SettingsDeferredResult>();
    ready_ = receiver_ && receiver_->state() != ReceiverState::Error
        && receiver_->state() != ReceiverState::Starting;
    awaitingRecordingResult_ = receiver_ && receiver_->recordingState() != RecordingState::Idle;
    if (receiver_) {
        connect(receiver_, &AirPlayReceiver::stateChanged, this, &SettingsApplyCoordinator::receiverStateChanged);
        connect(receiver_, &AirPlayReceiver::recordingStateChanged, this, &SettingsApplyCoordinator::recordingStateChanged);
        connect(receiver_, &AirPlayReceiver::recordingFinished, this, [this] { recordingTerminated(); });
        connect(receiver_, &AirPlayReceiver::recordingFailed, this, [this] { recordingTerminated(); });
        connect(receiver_, &QObject::destroyed, this, [this] { endReceiverLifecycle(); });
    }
    connect(deferrer_.get(), &SettingsChangeDeferrer::receiverConfigurationReady, this,
        [this] { consumeReady(epoch_, version_); });
}
SettingsApplyCoordinator::~SettingsApplyCoordinator() { endReceiverLifecycle(); }

bool SettingsApplyCoordinator::sessionActive() const {
    return receiver_ && (receiver_->state() == ReceiverState::Connected || receiver_->state() == ReceiverState::Connecting);
}
bool SettingsApplyCoordinator::recordingBlocked() const {
    return receiver_ && (receiver_->recordingState() != RecordingState::Idle || awaitingRecordingResult_);
}
void SettingsApplyCoordinator::receiverStateChanged(ReceiverState state) {
    if (applying_) return;
    if (state == ReceiverState::Starting && preparedStart_) { preparedStart_ = false; return; }
    if (state == ReceiverState::Error || state == ReceiverState::Starting) {
        const bool notify = state == ReceiverState::Error && pendingReceiverBatch_.has_value();
        endReceiverLifecycle();
        if (notify) {
            SettingsDeferredResult result = SettingsDeferredNotApplied{SettingsNotAppliedReason::ReceiverError,
                delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver settings have not been applied. The saved configuration is kept for the next receiver start."))};
            SettingsDiagnostics::recordDeferredResult(diagnosticSink_, result);
            emit deferredApplyFinished(result);
        }
        return;
    }
    if (state == ReceiverState::Discoverable || state == ReceiverState::Connecting || state == ReceiverState::Connected) preparedStart_ = false;
    if (ready_ && pendingReceiverBatch_ && !sessionActive())
        deferrer_->receiverSessionChanged(true, false);
}
void SettingsApplyCoordinator::recordingStateChanged(RecordingState state) {
    if (state == RecordingState::Recording) ++recordingVersion_;
    if (state != RecordingState::Idle) awaitingRecordingResult_ = true;
    // Idle precedes terminal delivery; release waits for qualified presentation completion.
}
void SettingsApplyCoordinator::recordingTerminated() {
    if (!receiver_ || receiver_->recordingState() != RecordingState::Idle || !awaitingRecordingResult_) return;
    const auto epoch = epoch_;
    const auto recording = recordingVersion_;
    const auto presentation = ++presentationVersion_;
    const QPointer<SettingsApplyCoordinator> owner(this);
    RecordingPresentationCompletion complete = [owner, epoch, recording, presentation] {
        if (!owner || owner->epoch_ != epoch || owner->recordingVersion_ != recording
            || owner->presentationVersion_ != presentation || !owner->awaitingRecordingResult_
            || !owner->receiver_ || owner->receiver_->recordingState() != RecordingState::Idle) return;
        owner->awaitingRecordingResult_ = false;
        if (owner->ready_ && owner->pendingReceiverBatch_)
            owner->deferrer_->recordingStateChanged(RecordingState::Finalizing, RecordingState::Idle);
    };
    // The UI owns only presentation work. The qualified completion owns no raw module pointer.
    if (presentationDispatcher_) presentationDispatcher_(std::move(complete));
    else complete();
}
void SettingsApplyCoordinator::consumeReady(quint64 epoch, quint64 version) {
    if (epoch != epoch_ || version != version_ || !ready_ || !pendingReceiverBatch_) return;
    if (busy_) {
        // Nested modal loops may deliver existing MetaCalls, but cannot execute or retry work.
        readyWhileBusy_ = std::make_pair(epoch, version);
        return;
    }
    if (recordingBlocked() || (pendingTiming_ == ReceiverApplyTiming::AfterDisconnect && sessionActive())) {
        deferrer_->deferReceiverConfiguration(*pendingReceiverBatch_,
            pendingTiming_ == ReceiverApplyTiming::AfterDisconnect && sessionActive(), recordingBlocked());
        return;
    }
    busy_ = true;
    operationEpoch_ = epoch_;
    const auto batch = *pendingReceiverBatch_;
    auto outcome = completeDeferredReceiverApplyImpl(batch, current_);
    outcome.committedSettings = current_;
    finishOperation();
    if (epoch != epoch_) return;
    SettingsDeferredResult result = outcome;
    SettingsDiagnostics::recordDeferredResult(diagnosticSink_, result);
    emit deferredApplyFinished(result);
}
void SettingsApplyCoordinator::finishOperation() {
    busy_ = false;
    const auto ready = std::exchange(readyWhileBusy_, std::nullopt);
    if (!ready || ready->first != epoch_ || ready->second != version_
        || !ready_ || !pendingReceiverBatch_ || posted_ == ready) return;
    posted_ = ready;
    QMetaObject::invokeMethod(this, [this, token = *ready] {
        if (posted_ == token) posted_.reset();
        consumeReady(token.first, token.second);
    }, Qt::QueuedConnection);
}
ReceiverConfigurationBatchResult SettingsApplyCoordinator::invokeBackend(const ReceiverConfigurationBatchRequest &batch) {
    applying_ = true;
    if (activeSubmit_) activeSubmit_->backendInvoked = true;
    const auto result = receiver_->applyConfigurationBatch(batch);
    applying_ = false;
    if (activeSubmit_) activeSubmit_->backendResult = result;
    return result;
}
SettingsSubmitResult SettingsApplyCoordinator::apply(const AppSettings &draft, const TimingChooser &chooser) {
    SettingsSubmitResult result;
    if (busy_) {
        result.status = SettingsSubmitStatus::Busy;
        result.userReason = delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Settings are being applied. Try again after the current operation finishes."));
        return result;
    }
    busy_ = true;
    operationEpoch_ = epoch_;
    auto request = plan(current_, draft, ready_ && sessionActive(), receiver_ ? receiver_->recordingState() : RecordingState::Idle);
    auto timing = std::optional<ReceiverApplyTiming>(ReceiverApplyTiming::Immediate);
    timingAuthorized_ = false;
    if (request.requiresReceiverTimingDecision) {
        if (!chooser) {
            result.status = SettingsSubmitStatus::TimingSelectionRequired;
        } else {
            timing = chooser();
            result.status = timing ? SettingsSubmitStatus::Completed : SettingsSubmitStatus::Cancelled;
            timingAuthorized_ = timing.has_value();
        }
        if (operationEpoch_ != epoch_) result.status = SettingsSubmitStatus::Interrupted;
        if (result.status != SettingsSubmitStatus::Completed) {
            finishOperation();
            result.userReason = submitFailureReason(result);
            SettingsDiagnostics::recordSubmitResult(diagnosticSink_, result);
            return result;
        }
    }
    // A modal chooser may have committed independent settings or changed the receiver state.
    request = plan(current_, draft, ready_ && sessionActive(), receiver_ ? receiver_->recordingState() : RecordingState::Idle);
    result.status = SettingsSubmitStatus::Completed;
    activeSubmit_ = &result;
    result.outcome = executePlan(request, *timing);
    result.outcome->committedSettings = current_;
    activeSubmit_ = nullptr;
    if (operationEpoch_ != epoch_) {
        result.status = SettingsSubmitStatus::Interrupted;
        // The backend result is authoritative, but a cancelled compensation is not a rollback outcome.
        if (!result.backendResult || result.backendResult->status != ReceiverConfigurationBatchStatus::Applied) {
            auto &fields = result.outcome->fieldResults;
            fields.erase(std::remove_if(fields.begin(), fields.end(), [](const SettingsFieldResult &field) {
                return isReceiverField(field.field) && field.status != SettingsFieldStatus::ValidationFailed;
            }), fields.end());
        }
        result.outcome->mayClose = false;
    }
    if (result.status == SettingsSubmitStatus::Completed)
        SettingsDiagnostics::recordApplyOutcome(diagnosticSink_, *result.outcome, "apply", *timing);
    else {
        result.userReason = submitFailureReason(result);
        SettingsDiagnostics::recordSubmitResult(diagnosticSink_, result);
    }
    finishOperation();
    return result;
}
ReceiverStartPreparationResult SettingsApplyCoordinator::prepareReceiverStart() {
    ReceiverStartPreparationResult result;
    if (!receiver_) {
        result.status = ReceiverStartPreparationStatus::Unavailable;
        result.userReason = delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "The receiver is unavailable."));
        return result;
    }
    if (busy_ || receiver_->state() != ReceiverState::Idle) {
        result.status = ReceiverStartPreparationStatus::NotIdle;
        result.userReason = delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver preparation requires an idle receiver."));
        return result;
    }
    endReceiverLifecycle();
    busy_ = true;
    operationEpoch_ = epoch_;
    const auto batch = mergeSavedReceiverTarget(current_, {});
    if (hasReceiverChanges(batch)) result.backendResult = invokeBackend(batch);
    finishOperation();
    if (operationEpoch_ != epoch_ || (result.backendResult && result.backendResult->status != ReceiverConfigurationBatchStatus::Applied)) {
        result.status = ReceiverStartPreparationStatus::BackendFailure;
        result.userReason = delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver preparation did not complete."));
        return result;
    }
    ready_ = true;
    preparedStart_ = true;
    awaitingRecordingResult_ = receiver_->recordingState() != RecordingState::Idle;
    result.status = ReceiverStartPreparationStatus::Prepared;
    return result;
}
void SettingsApplyCoordinator::endReceiverLifecycle() {
    ++epoch_;
    ++version_;
    ready_ = false;
    preparedStart_ = false;
    timingSelectionPending_ = false;
    pendingReceiverBatch_.reset();
    posted_.reset();
    readyWhileBusy_.reset();
    deferrer_->cancelPendingReceiverConfiguration();
}

SettingsApplyPlan SettingsApplyCoordinator::plan(const AppSettings &baseline,
                                                 const AppSettings &candidate,
                                                 bool receiverSessionActive,
                                                 RecordingState recordingState) const {
    SettingsApplyPlan result;
    result.baseline = baseline;
    result.candidate = candidate;
    result.receiverSessionActive = receiverSessionActive;
    result.recordingIdle = recordingState == RecordingState::Idle;

    const QVector<SettingsFieldId> fields = allSettingsFields();
    result.validationResults.reserve(fields.size());
    for (const SettingsFieldId &field : fields) {
        result.validationResults.append({field, settingsFieldValue(candidate, field)});
    }

    if (candidate.receiverName().trimmed().isEmpty()) {
        markValidationFailure(mutableResultForField(&result.validationResults,
                                                    SettingsFieldId::receiverName()),
                              QStringLiteral("Receiver name cannot be empty."),
                              QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver name cannot be empty."));
    }

    QHash<QString, QVector<SettingsFieldId>> shortcutFieldsBySequence;
    for (const SettingsFieldId &field : fields) {
        if (field.kind != SettingsFieldKind::Shortcut || !field.shortcutAction.has_value()) {
            continue;
        }

        const QKeySequence sequence(candidate.shortcutFor(*field.shortcutAction));
        SettingsFieldResult *fieldResult = mutableResultForField(&result.validationResults, field);
        if (sequence.isEmpty()) {
            markValidationFailure(fieldResult, QStringLiteral("Shortcut cannot be empty."),
                                  QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut cannot be empty."));
            continue;
        }
        if (sequence.count() > 1) {
            markValidationFailure(fieldResult,
                                  QStringLiteral("Shortcut must use a single key combination."),
                                  QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut must use a single key combination."));
            continue;
        }
        if (!WindowsHotkeyService::toNativeHotkey(sequence).has_value()) {
            markValidationFailure(fieldResult,
                                  QStringLiteral("Shortcut is not supported as a Windows global hotkey."),
                                  QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut is not supported as a Windows global hotkey."));
            continue;
        }

        shortcutFieldsBySequence[sequence.toString(QKeySequence::PortableText)].append(field);
    }

    for (auto sequence = shortcutFieldsBySequence.cbegin(); sequence != shortcutFieldsBySequence.cend();
         ++sequence) {
        if (sequence.value().size() < 2) {
            continue;
        }

        const QKeySequence duplicate(sequence.key());
        const QString reason = QStringLiteral("Shortcut %1 is assigned to more than one action.")
                                   .arg(duplicate.toString(QKeySequence::NativeText));
        for (const SettingsFieldId &field : sequence.value()) {
            markValidationFailure(mutableResultForField(&result.validationResults, field), reason,
                                  QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut %1 is assigned to more than one action."),
                                  {duplicate.toString(QKeySequence::NativeText)});
        }
    }

    for (const SettingsFieldId &field : fields) {
        if (!isReceiverField(field)) {
            continue;
        }

        const SettingsFieldResult *fieldResult = resultForField(result.validationResults, field);
        if (fieldResult != nullptr && fieldResult->status == SettingsFieldStatus::Unchanged
            && settingsFieldValue(baseline, field) != settingsFieldValue(candidate, field)) {
            result.validChangedReceiverFields.append(field);
        }
    }

    if (timingSelectionPending_ && receiver_) {
        const auto outstanding = mergeSavedReceiverTarget(baseline, {});
        for (const auto &field : allSettingsFields()) {
            const auto *validation = resultForField(result.validationResults, field);
            if (batchChangesField(outstanding, field) && validation
                && validation->status != SettingsFieldStatus::ValidationFailed
                && !result.validChangedReceiverFields.contains(field)) result.validChangedReceiverFields.append(field);
        }
    }
    AppSettings acceptedTarget = baseline;
    for (const auto &field : result.validChangedReceiverFields) copySettingsField(candidate, field, &acceptedTarget);
    result.requiresReceiverTimingDecision = receiverSessionActive
        && !result.validChangedReceiverFields.isEmpty()
        && hasReceiverChanges(mergeSavedReceiverTarget(acceptedTarget, receiverBatchForPlan(result)));
    return result;
}

ReceiverConfigurationBatchRequest SettingsApplyCoordinator::mergeSavedReceiverTarget(
    const AppSettings &savedTarget, const ReceiverConfigurationBatchRequest &delta) const {
    ReceiverConfigurationBatchRequest merged = delta;
    if (pendingReceiverBatch_.has_value()) {
        merged.rollbackReceiverName = pendingReceiverBatch_->rollbackReceiverName;
        merged.rollbackVideoQuality = pendingReceiverBatch_->rollbackVideoQuality;
    } else if (receiver_ != nullptr) {
        merged.rollbackReceiverName = receiver_->receiverName();
        merged.rollbackVideoQuality = receiver_->videoQuality();
    }
    merged.requestedReceiverName = savedTarget.receiverName();
    merged.requestedVideoQuality = savedTarget.videoQuality();
    merged.receiverNameChanged = merged.requestedReceiverName != merged.rollbackReceiverName;
    merged.resolutionChanged =
        merged.requestedVideoQuality.resolution != merged.rollbackVideoQuality.resolution;
    merged.frameRateChanged =
        merged.requestedVideoQuality.frameRate != merged.rollbackVideoQuality.frameRate;
    return merged;
}

SettingsApplyOutcome SettingsApplyCoordinator::executePlan(const SettingsApplyPlan &plan,
                                                           ReceiverApplyTiming timing) {
    SettingsApplyOutcome outcome;
    outcome.committedSettings = plan.baseline;
    outcome.fieldResults = plan.validationResults;
    AppSettings prospective = plan.baseline;

    struct SuccessfulChangedShortcut {
        SettingsFieldId field;
        ShortcutAction action;
        QKeySequence baseline;
    };
    QVector<SuccessfulChangedShortcut> changedShortcuts;

    QVector<SettingsFieldId> validShortcutFields;
    QVector<HotkeyRegistrationRequest> shortcutRequests;
    for (const SettingsFieldId &field : allSettingsFields()) {
        if (!isShortcutField(field)) {
            continue;
        }
        SettingsFieldResult *fieldResult = mutableResultForField(&outcome.fieldResults, field);
        if (fieldResult == nullptr || fieldResult->status == SettingsFieldStatus::ValidationFailed) {
            continue;
        }
        const ShortcutAction action = *field.shortcutAction;
        validShortcutFields.append(field);
        shortcutRequests.append({action, QKeySequence(plan.candidate.shortcutFor(action))});
    }

    if (hotkeys_ == nullptr) {
        for (const SettingsFieldId &field : validShortcutFields) {
            SettingsFieldResult *fieldResult = mutableResultForField(&outcome.fieldResults, field);
            const bool changed = fieldsDiffer(plan.baseline, plan.candidate, field);
            copySettingsField(changed ? plan.candidate : plan.baseline, field, &prospective);
            fieldResult->status = changed ? SettingsFieldStatus::Applied
                                          : SettingsFieldStatus::Unchanged;
        }
    } else {
        const QVector<HotkeyActionRegistrationResult> registrations =
            hotkeys_->registerShortcuts(shortcutRequests);
        for (qsizetype index = 0; index < validShortcutFields.size(); ++index) {
            const SettingsFieldId &field = validShortcutFields.at(index);
            SettingsFieldResult *fieldResult = mutableResultForField(&outcome.fieldResults, field);
            const bool changed = fieldsDiffer(plan.baseline, plan.candidate, field);
            const HotkeyRegistrationResult registration = index < registrations.size()
                ? registrations.at(index).registration
                : HotkeyRegistrationResult{false, false,
                    HotkeyError{std::nullopt,
                                QStringLiteral("The hotkey service did not return a result.")}};
            if (!registration.registered) {
                setHotkeyFailure(fieldResult, registration);
                continue;
            }

            copySettingsField(changed ? plan.candidate : plan.baseline, field, &prospective);
            fieldResult->status = changed ? SettingsFieldStatus::Applied
                                          : SettingsFieldStatus::Unchanged;
            if (changed && !registration.unchanged) {
                changedShortcuts.append({field, *field.shortcutAction,
                                         QKeySequence(plan.baseline.shortcutFor(*field.shortcutAction))});
            }
        }
    }

    if (operationEpoch_ != epoch_) {
        outcome.committedSettings = current_;
        return outcome;
    }
    prospective.setVolume(current_.volume());
    prospective.setAspectRatioLock(current_.aspectRatioLock());
    prospective.setVideoFitMode(current_.videoFitMode());
    for (const SettingsFieldId &field : allSettingsFields()) {
        if (isShortcutField(field)) {
            continue;
        }
        SettingsFieldResult *fieldResult = mutableResultForField(&outcome.fieldResults, field);
        if (fieldResult == nullptr || fieldResult->status == SettingsFieldStatus::ValidationFailed) {
            continue;
        }

        const bool changed = fieldsDiffer(plan.baseline, plan.candidate, field);
        copySettingsField(changed ? plan.candidate : plan.baseline, field, &prospective);
        fieldResult->status = changed ? SettingsFieldStatus::Applied
                                      : SettingsFieldStatus::Unchanged;
    }

    const auto saved = saveAndAdopt(current_, prospective, persistence_, diagnosticSink_, "apply",
        [this] { return operationEpoch_ == epoch_; });
    const auto &persistenceResult = saved.result;
    if (activeSubmit_) activeSubmit_->settingsSaved = saved.firstWriteSucceeded;
    outcome.committedSettings = current_;
    if (saved.firstWriteSucceeded && !persistenceResult.success) {
        // A successful shared write only supersedes receiver work when its
        // validated receiver goal changes. Preserve unchanged work and gates.
        const auto delta = receiverBatchForPlan(plan);
        if (hasReceiverChanges(delta)) {
            ++version_;
            pendingReceiverBatch_.reset();
            readyWhileBusy_.reset();
            deferrer_->cancelPendingReceiverConfiguration();
            timingSelectionPending_ = true;
        }
        outcome.globalResult = SettingsApplyGlobalResult{SettingsApplyGlobalStatus::PersistenceFailed, persistenceResult};
        markUnconfirmedSavedState(&outcome, mergeSavedReceiverTarget(current_, receiverBatchForPlan(plan)));
        updateMayClose(&outcome);
        return outcome;
    }
    if (persistenceResult.success) {
        const ReceiverConfigurationBatchRequest delta = receiverBatchForPlan(plan);
        if (!hasReceiverChanges(delta)) {
            updateMayClose(&outcome);
            return outcome;
        }
        timingSelectionPending_ = false;
        const ReceiverConfigurationBatchRequest batch = mergeSavedReceiverTarget(current_, delta);
        if (!hasReceiverChanges(batch)) {
            pendingReceiverBatch_.reset();
            if (deferrer_ != nullptr) {
                deferrer_->cancelPendingReceiverConfiguration();
            }
            updateMayClose(&outcome);
            return outcome;
        }
        ++version_;
        deferrer_->cancelPendingReceiverConfiguration();
        pendingReceiverBatch_ = batch;
        pendingTiming_ = timing;
        if (operationEpoch_ != epoch_ || (receiver_ && !ready_)) {
            pendingReceiverBatch_.reset();
            markReceiverFieldsDeferred(&outcome, batch);
            for (auto &field : outcome.fieldResults) if (field.status == SettingsFieldStatus::Deferred)
                field.userReason = delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Receiver settings are saved for the next receiver start."));
            updateMayClose(&outcome);
            return outcome;
        }
        if (receiver_ && sessionActive() && !timingAuthorized_) {
            pendingReceiverBatch_.reset();
            timingSelectionPending_ = true;
            if (activeSubmit_) activeSubmit_->status = SettingsSubmitStatus::TimingSelectionRequired;
            markReceiverFieldsDeferred(&outcome, batch);
            updateMayClose(&outcome);
            return outcome;
        }
        const bool waitSession = timing == ReceiverApplyTiming::AfterDisconnect && sessionActive();
        const bool waitRecording = recordingBlocked();
        if (receiver_ && (waitSession || waitRecording)) {
            deferrer_->deferReceiverConfiguration(batch, waitSession, waitRecording);
            markReceiverFieldsDeferred(&outcome, batch);
            if (timing == ReceiverApplyTiming::Immediate && waitRecording) receiver_->stopRecording();
            updateMayClose(&outcome);
            return outcome;
        }

        if (receiver_ == nullptr) {
            ReceiverConfigurationBatchResult unavailable;
            unavailable.applyError = QStringLiteral("AirPlay receiver is unavailable");
            unavailable.recoveryError = QStringLiteral("No runtime receiver state is available");
            unavailable.knownRuntimeReceiverName = QStringLiteral("unavailable");
            unavailable.knownRuntimeVideoQuality = batch.rollbackVideoQuality;
            pendingReceiverBatch_.reset();
            if (deferrer_ != nullptr) {
                deferrer_->cancelPendingReceiverConfiguration();
            }
            compensateUnavailableReceiver(&outcome, batch, current_, persistence_, unavailable, diagnosticSink_,
                [this] { return operationEpoch_ == epoch_; });
            updateMayClose(&outcome);
            return outcome;
        }

        pendingReceiverBatch_.reset();
        if (deferrer_ != nullptr) {
            deferrer_->cancelPendingReceiverConfiguration();
        }
        const ReceiverConfigurationBatchResult result = invokeBackend(batch);
        if (operationEpoch_ != epoch_) return outcome;
        if (result.status == ReceiverConfigurationBatchStatus::Applied) {
            updateMayClose(&outcome);
            return outcome;
        }
        if (result.status == ReceiverConfigurationBatchStatus::RecoveryFailed) {
            markReceiverRecoveryFailure(&outcome, batch, result, describeRequestedConfiguration(batch));
            updateMayClose(&outcome);
            return outcome;
        }

        AppSettings compensated = current_;
        restoreBatchFields(batch, &compensated);
        const auto saved = saveAndAdopt(current_, compensated, persistence_, diagnosticSink_, "compensation",
            [this] { return operationEpoch_ == epoch_; });
        const auto &compensation = saved.result;
        outcome.committedSettings = current_;
        if (compensation.success) {
            markReceiverRollback(&outcome, batch, result);
        } else {
            outcome.globalResult = SettingsApplyGlobalResult{
                SettingsApplyGlobalStatus::PersistenceFailed, compensation};
            if (saved.firstWriteSucceeded) markUnconfirmedSavedState(&outcome, batch, &result);
            else markReceiverCompensationFailure(&outcome, batch, result, compensation);
        }
        updateMayClose(&outcome);
        return outcome;
    }

    QVector<HotkeyRegistrationRequest> restorationRequests;
    QVector<SuccessfulChangedShortcut> restoredShortcuts;
    restorationRequests.reserve(changedShortcuts.size());
    restoredShortcuts.reserve(changedShortcuts.size());
    for (auto shortcut = changedShortcuts.crbegin(); shortcut != changedShortcuts.crend(); ++shortcut) {
        restorationRequests.append({shortcut->action, shortcut->baseline});
        restoredShortcuts.append(*shortcut);
    }
    const QVector<HotkeyActionRegistrationResult> restorations =
        restorationRequests.isEmpty() ? QVector<HotkeyActionRegistrationResult>{}
                                      : hotkeys_->registerShortcuts(restorationRequests);
    for (qsizetype index = 0; index < restoredShortcuts.size(); ++index) {
        const SuccessfulChangedShortcut &shortcut = restoredShortcuts.at(index);
        const HotkeyRegistrationResult restoration = index < restorations.size()
            ? restorations.at(index).registration
            : HotkeyRegistrationResult{false, false,
                HotkeyError{std::nullopt,
                            QStringLiteral("The hotkey service did not return a restoration result.")}};
        if (restoration.registered) {
            continue;
        }

        SettingsFieldResult *fieldResult = mutableResultForField(&outcome.fieldResults, shortcut.field);
        if (fieldResult == nullptr) {
            continue;
        }
        fieldResult->status = SettingsFieldStatus::RecoveryFailed;
        fieldResult->reason = QStringLiteral("Not committed because the settings file could not be saved.");
        fieldResult->userReason = delayedMessage(
            QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Not committed because the settings file could not be saved."));
        fieldResult->recoveryError = QStringLiteral("Baseline shortcut restoration failed for %1.")
            .arg(shortcutDescription(*fieldResult)) + compensationFailureDescription(restoration);
        const QString rawDetails = rawHotkeyFailureDetails(restoration);
        fieldResult->userRecoveryError = rawDetails.isEmpty()
            ? delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut restoration failed."))
            : delayedMessage(QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Shortcut restoration failed: %1"),
                             {rawDetails});
    }

    for (SettingsFieldResult &fieldResult : outcome.fieldResults) {
        if (fieldResult.status == SettingsFieldStatus::ValidationFailed) {
            continue;
        }
        if (fieldsDiffer(plan.baseline, plan.candidate, fieldResult.field)
            && fieldResult.status != SettingsFieldStatus::RecoveryFailed) {
            fieldResult.status = SettingsFieldStatus::ApplyFailedRolledBack;
            fieldResult.reason = QStringLiteral("Not committed because the settings file could not be saved.");
            fieldResult.userReason = delayedMessage(
                QT_TRANSLATE_NOOP("SettingsApplyCoordinator", "Not committed because the settings file could not be saved."));
        } else if (!fieldsDiffer(plan.baseline, plan.candidate, fieldResult.field)) {
            fieldResult.status = SettingsFieldStatus::Unchanged;
        }
    }

    outcome.committedSettings = current_;
    outcome.globalResult = SettingsApplyGlobalResult{
        SettingsApplyGlobalStatus::PersistenceFailed,
        persistenceResult,
    };
    return outcome;
}

SettingsApplyOutcome SettingsApplyCoordinator::completeDeferredReceiverApplyImpl(
    const ReceiverConfigurationBatchRequest &batch,
    const AppSettings &currentlyCommitted) {
    const ReceiverConfigurationBatchRequest effectiveBatch = pendingReceiverBatch_.value_or(batch);
    pendingReceiverBatch_.reset();
    SettingsApplyOutcome outcome = makeDeferredCompletionOutcome(currentlyCommitted);
    if (!hasReceiverChanges(effectiveBatch)) {
        updateMayClose(&outcome);
        return outcome;
    }

    if (receiver_ == nullptr) {
        ReceiverConfigurationBatchResult unavailable;
        unavailable.applyError = QStringLiteral("AirPlay receiver is unavailable");
        unavailable.recoveryError = QStringLiteral("No runtime receiver state is available");
        unavailable.knownRuntimeReceiverName = QStringLiteral("unavailable");
        unavailable.knownRuntimeVideoQuality = effectiveBatch.rollbackVideoQuality;
        compensateUnavailableReceiver(&outcome, effectiveBatch, current_, persistence_, unavailable, diagnosticSink_,
            [this] { return operationEpoch_ == epoch_; });
        updateMayClose(&outcome);
        return outcome;
    }

    const ReceiverConfigurationBatchResult result = invokeBackend(effectiveBatch);
    if (operationEpoch_ != epoch_) return outcome;
    if (result.status == ReceiverConfigurationBatchStatus::Applied) {
        for (SettingsFieldResult &fieldResult : outcome.fieldResults) {
            if (batchChangesField(effectiveBatch, fieldResult.field)) {
                fieldResult.status = SettingsFieldStatus::Applied;
            }
        }
        updateMayClose(&outcome);
        return outcome;
    }
    if (result.status == ReceiverConfigurationBatchStatus::RecoveryFailed) {
        markReceiverRecoveryFailure(&outcome, effectiveBatch, result,
                                    describeRequestedConfiguration(effectiveBatch));
        updateMayClose(&outcome);
        return outcome;
    }

    AppSettings compensated = current_;
    restoreBatchFields(effectiveBatch, &compensated);
    const auto saved = saveAndAdopt(current_, compensated, persistence_, diagnosticSink_, "compensation",
        [this] { return operationEpoch_ == epoch_; });
    const auto &compensation = saved.result;
    outcome.committedSettings = current_;
    if (compensation.success) {
        markReceiverRollback(&outcome, effectiveBatch, result);
    } else {
        outcome.globalResult = SettingsApplyGlobalResult{
            SettingsApplyGlobalStatus::PersistenceFailed, compensation};
        if (saved.firstWriteSucceeded) markUnconfirmedSavedState(&outcome, effectiveBatch, &result);
        else markReceiverCompensationFailure(&outcome, effectiveBatch, result, compensation);
    }
    updateMayClose(&outcome);
    return outcome;
}

#include "app/SettingsApplyCoordinator.h"

#include "app/SettingsChangeDeferrer.h"
#include "backend/AirPlayReceiver.h"
#include "platform/HotkeyService.h"
#include "platform/WindowsHotkeyService.h"

#include <algorithm>
#include <QHash>

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

void markValidationFailure(SettingsFieldResult *result, const QString &reason) {
    if (result == nullptr) {
        return;
    }
    result->status = SettingsFieldStatus::ValidationFailed;
    result->reason = reason;
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

    if (registration.previousRestored) {
        fieldResult->status = SettingsFieldStatus::ApplyFailedRolledBack;
        fieldResult->reason += QStringLiteral(" The previous shortcut was restored.");
        return;
    }

    fieldResult->status = SettingsFieldStatus::RecoveryFailed;
    fieldResult->recoveryError = QStringLiteral(
        "The previous shortcut could not be restored; %1 currently has no confirmed global shortcut.")
        .arg(action);
    if (registration.recoveryError.has_value()) {
        fieldResult->recoveryError += QStringLiteral(" Recovery failed: %1")
            .arg(describeHotkeyError(*registration.recoveryError));
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
        if (batchChangesField(batch, fieldResult.field)) {
            fieldResult.status = SettingsFieldStatus::Deferred;
            fieldResult.reason = QStringLiteral("Receiver configuration will be applied when its blocker clears.");
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
        if (!batchChangesField(batch, fieldResult.field)) {
            continue;
        }
        fieldResult.status = SettingsFieldStatus::RecoveryFailed;
        fieldResult.reason = QStringLiteral("Receiver configuration apply failed: %1. Known saved state is %2.")
            .arg(applyError, knownSavedState);
        fieldResult.recoveryError = QStringLiteral(
            "Receiver restoration failed: %1. Known runtime state is %2 and cannot be confirmed.")
            .arg(result.recoveryError.isEmpty()
                     ? QStringLiteral("the rollback configuration did not complete")
                     : result.recoveryError,
                 runtime);
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
        if (batchChangesField(batch, fieldResult.field)) {
            fieldResult.status = SettingsFieldStatus::ApplyFailedRolledBack;
            fieldResult.reason = QStringLiteral(
                "Receiver configuration apply failed: %1. The previous receiver configuration was restored.")
                .arg(applyError);
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
        if (!batchChangesField(batch, fieldResult.field)) {
            continue;
        }
        fieldResult.status = SettingsFieldStatus::RecoveryFailed;
        fieldResult.reason = QStringLiteral(
            "Receiver configuration apply failed: %1. Known saved state is %2; the compensating save did not complete.")
            .arg(applyError, describeRequestedConfiguration(batch));
        fieldResult.recoveryError = QStringLiteral(
            "Receiver runtime rollback completed. Known runtime state is %1. Compensating JSON save failed: %2. "
            "The saved state cannot be confirmed.")
            .arg(runtime, compensation.errorString);
    }
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

SettingsApplyCoordinator::SettingsApplyCoordinator(HotkeyService *hotkeys,
                                                   SettingsPersistence *persistence,
                                                   AirPlayReceiver *receiver,
                                                   SettingsChangeDeferrer *deferrer)
    : hotkeys_(hotkeys),
      persistence_(persistence),
      receiver_(receiver),
      deferrer_(deferrer) {
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
                              QStringLiteral("Receiver name cannot be empty."));
    }

    QHash<QString, QVector<SettingsFieldId>> shortcutFieldsBySequence;
    for (const SettingsFieldId &field : fields) {
        if (field.kind != SettingsFieldKind::Shortcut || !field.shortcutAction.has_value()) {
            continue;
        }

        const QKeySequence sequence(candidate.shortcutFor(*field.shortcutAction));
        SettingsFieldResult *fieldResult = mutableResultForField(&result.validationResults, field);
        if (sequence.isEmpty()) {
            markValidationFailure(fieldResult, QStringLiteral("Shortcut cannot be empty."));
            continue;
        }
        if (sequence.count() > 1) {
            markValidationFailure(fieldResult,
                                  QStringLiteral("Shortcut must use a single key combination."));
            continue;
        }
        if (!WindowsHotkeyService::toNativeHotkey(sequence).has_value()) {
            markValidationFailure(fieldResult,
                                  QStringLiteral("Shortcut is not supported as a Windows global hotkey."));
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
            markValidationFailure(mutableResultForField(&result.validationResults, field), reason);
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

    result.requiresReceiverTimingDecision = receiverSessionActive
        && !result.validChangedReceiverFields.isEmpty();
    return result;
}

SettingsApplyOutcome SettingsApplyCoordinator::execute(const SettingsApplyPlan &plan,
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

    for (const SettingsFieldId &field : allSettingsFields()) {
        if (!isShortcutField(field)) {
            continue;
        }
        SettingsFieldResult *fieldResult = mutableResultForField(&outcome.fieldResults, field);
        if (fieldResult == nullptr || fieldResult->status == SettingsFieldStatus::ValidationFailed) {
            continue;
        }

        const bool changed = fieldsDiffer(plan.baseline, plan.candidate, field);
        if (hotkeys_ == nullptr) {
            copySettingsField(changed ? plan.candidate : plan.baseline, field, &prospective);
            fieldResult->status = changed ? SettingsFieldStatus::Applied
                                          : SettingsFieldStatus::Unchanged;
            continue;
        }

        const ShortcutAction action = *field.shortcutAction;
        const QKeySequence candidate(plan.candidate.shortcutFor(action));
        const HotkeyRegistrationResult registration = hotkeys_->registerShortcut(action, candidate);
        if (!registration.registered) {
            setHotkeyFailure(fieldResult, registration);
            continue;
        }

        copySettingsField(changed ? plan.candidate : plan.baseline, field, &prospective);
        fieldResult->status = changed ? SettingsFieldStatus::Applied
                                      : SettingsFieldStatus::Unchanged;
        if (changed && !registration.unchanged) {
            changedShortcuts.append({field, action,
                                     QKeySequence(plan.baseline.shortcutFor(action))});
        }
    }

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

    const AppSettingsSaveResult persistenceResult = persistence_ == nullptr
        ? AppSettingsSaveResult{true}
        : persistence_->save(prospective);
    if (persistenceResult.success) {
        outcome.committedSettings = prospective;
        const ReceiverConfigurationBatchRequest batch = receiverBatchForPlan(plan);
        if (!hasReceiverChanges(batch)) {
            updateMayClose(&outcome);
            return outcome;
        }

        if (timing == ReceiverApplyTiming::AfterDisconnect) {
            if (deferrer_ != nullptr) {
                deferrer_->deferReceiverConfiguration(batch, true, !plan.recordingIdle);
                markReceiverFieldsDeferred(&outcome, batch);
            } else {
                ReceiverConfigurationBatchResult unavailable;
                unavailable.applyError = QStringLiteral("Receiver configuration deferrer is unavailable");
                unavailable.recoveryError = QStringLiteral("No deferred receiver configuration was scheduled");
                unavailable.knownRuntimeReceiverName = receiver_ == nullptr ? QStringLiteral("unavailable")
                                                                            : receiver_->receiverName();
                unavailable.knownRuntimeVideoQuality = receiver_ == nullptr
                    ? batch.rollbackVideoQuality : receiver_->videoQuality();
                markReceiverRecoveryFailure(&outcome, batch, unavailable,
                                            describeRequestedConfiguration(batch));
            }
            updateMayClose(&outcome);
            return outcome;
        }

        if (!plan.recordingIdle) {
            if (receiver_ != nullptr && deferrer_ != nullptr) {
                receiver_->stopRecording();
                deferrer_->deferReceiverConfiguration(batch, false, true);
                markReceiverFieldsDeferred(&outcome, batch);
            } else {
                ReceiverConfigurationBatchResult unavailable;
                unavailable.applyError = receiver_ == nullptr
                    ? QStringLiteral("AirPlay receiver is unavailable")
                    : QStringLiteral("Receiver configuration deferrer is unavailable");
                unavailable.recoveryError = QStringLiteral("No deferred receiver configuration was scheduled");
                unavailable.knownRuntimeReceiverName = receiver_ == nullptr ? QStringLiteral("unavailable")
                                                                            : receiver_->receiverName();
                unavailable.knownRuntimeVideoQuality = receiver_ == nullptr
                    ? batch.rollbackVideoQuality : receiver_->videoQuality();
                markReceiverRecoveryFailure(&outcome, batch, unavailable,
                                            describeRequestedConfiguration(batch));
            }
            updateMayClose(&outcome);
            return outcome;
        }

        if (receiver_ == nullptr) {
            ReceiverConfigurationBatchResult unavailable;
            unavailable.applyError = QStringLiteral("AirPlay receiver is unavailable");
            unavailable.recoveryError = QStringLiteral("No runtime receiver state is available");
            unavailable.knownRuntimeReceiverName = QStringLiteral("unavailable");
            unavailable.knownRuntimeVideoQuality = batch.rollbackVideoQuality;
            markReceiverRecoveryFailure(&outcome, batch, unavailable,
                                        describeRequestedConfiguration(batch));
            updateMayClose(&outcome);
            return outcome;
        }

        const ReceiverConfigurationBatchResult result = receiver_->applyConfigurationBatch(batch);
        if (result.status == ReceiverConfigurationBatchStatus::Applied) {
            updateMayClose(&outcome);
            return outcome;
        }
        if (result.status == ReceiverConfigurationBatchStatus::RecoveryFailed) {
            markReceiverRecoveryFailure(&outcome, batch, result, describeRequestedConfiguration(batch));
            updateMayClose(&outcome);
            return outcome;
        }

        AppSettings compensated = prospective;
        restoreBatchFields(batch, &compensated);
        const AppSettingsSaveResult compensation = persistence_ == nullptr
            ? AppSettingsSaveResult{true} : persistence_->save(compensated);
        if (compensation.success) {
            outcome.committedSettings = compensated;
            markReceiverRollback(&outcome, batch, result);
        } else {
            outcome.globalResult = SettingsApplyGlobalResult{
                SettingsApplyGlobalStatus::PersistenceFailed, compensation};
            markReceiverCompensationFailure(&outcome, batch, result, compensation);
        }
        updateMayClose(&outcome);
        return outcome;
    }

    for (auto shortcut = changedShortcuts.crbegin(); shortcut != changedShortcuts.crend(); ++shortcut) {
        const HotkeyRegistrationResult restoration = hotkeys_->registerShortcut(shortcut->action,
                                                                                 shortcut->baseline);
        if (restoration.registered) {
            continue;
        }

        SettingsFieldResult *fieldResult = mutableResultForField(&outcome.fieldResults, shortcut->field);
        if (fieldResult == nullptr) {
            continue;
        }
        fieldResult->status = SettingsFieldStatus::RecoveryFailed;
        fieldResult->reason = QStringLiteral("Not committed because the settings file could not be saved.");
        fieldResult->recoveryError = QStringLiteral("Baseline shortcut restoration failed for %1.")
            .arg(shortcutDescription(*fieldResult)) + compensationFailureDescription(restoration);
    }

    for (SettingsFieldResult &fieldResult : outcome.fieldResults) {
        if (fieldResult.status == SettingsFieldStatus::ValidationFailed) {
            continue;
        }
        if (fieldsDiffer(plan.baseline, plan.candidate, fieldResult.field)
            && fieldResult.status != SettingsFieldStatus::RecoveryFailed) {
            fieldResult.status = SettingsFieldStatus::ApplyFailedRolledBack;
            fieldResult.reason = QStringLiteral("Not committed because the settings file could not be saved.");
        } else if (!fieldsDiffer(plan.baseline, plan.candidate, fieldResult.field)) {
            fieldResult.status = SettingsFieldStatus::Unchanged;
        }
    }

    outcome.committedSettings = plan.baseline;
    outcome.globalResult = SettingsApplyGlobalResult{
        SettingsApplyGlobalStatus::PersistenceFailed,
        persistenceResult,
    };
    return outcome;
}

SettingsApplyOutcome SettingsApplyCoordinator::completeDeferredReceiverApply(
    const ReceiverConfigurationBatchRequest &batch,
    const AppSettings &currentlyCommitted) {
    SettingsApplyOutcome outcome = makeDeferredCompletionOutcome(currentlyCommitted);
    if (!hasReceiverChanges(batch)) {
        updateMayClose(&outcome);
        return outcome;
    }

    if (receiver_ == nullptr) {
        ReceiverConfigurationBatchResult unavailable;
        unavailable.applyError = QStringLiteral("AirPlay receiver is unavailable");
        unavailable.recoveryError = QStringLiteral("No runtime receiver state is available");
        unavailable.knownRuntimeReceiverName = QStringLiteral("unavailable");
        unavailable.knownRuntimeVideoQuality = batch.rollbackVideoQuality;
        markReceiverRecoveryFailure(&outcome, batch, unavailable,
                                    describeRequestedConfiguration(batch));
        updateMayClose(&outcome);
        return outcome;
    }

    const ReceiverConfigurationBatchResult result = receiver_->applyConfigurationBatch(batch);
    if (result.status == ReceiverConfigurationBatchStatus::Applied) {
        for (SettingsFieldResult &fieldResult : outcome.fieldResults) {
            if (batchChangesField(batch, fieldResult.field)) {
                fieldResult.status = SettingsFieldStatus::Applied;
            }
        }
        updateMayClose(&outcome);
        return outcome;
    }
    if (result.status == ReceiverConfigurationBatchStatus::RecoveryFailed) {
        markReceiverRecoveryFailure(&outcome, batch, result,
                                    describeRequestedConfiguration(batch));
        updateMayClose(&outcome);
        return outcome;
    }

    AppSettings compensated = currentlyCommitted;
    restoreBatchFields(batch, &compensated);
    const AppSettingsSaveResult compensation = persistence_ == nullptr
        ? AppSettingsSaveResult{true} : persistence_->save(compensated);
    if (compensation.success) {
        outcome.committedSettings = compensated;
        markReceiverRollback(&outcome, batch, result);
    } else {
        outcome.globalResult = SettingsApplyGlobalResult{
            SettingsApplyGlobalStatus::PersistenceFailed, compensation};
        markReceiverCompensationFailure(&outcome, batch, result, compensation);
    }
    updateMayClose(&outcome);
    return outcome;
}

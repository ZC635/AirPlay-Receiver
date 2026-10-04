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
        if (batchChangesField(batch, fieldResult.field)) {
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
        if (!batchChangesField(batch, fieldResult.field)) {
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
        if (batchChangesField(batch, fieldResult.field)) {
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
        if (!batchChangesField(batch, fieldResult.field)) {
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
        if (!batchChangesField(batch, fieldResult.field)) {
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
        if (!batchChangesField(batch, fieldResult.field)) {
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

void compensateUnavailableReceiver(SettingsApplyOutcome *outcome,
                                   const ReceiverConfigurationBatchRequest &batch,
                                   const AppSettings &prospective,
                                   SettingsPersistence *persistence,
                                   const ReceiverConfigurationBatchResult &unavailable,
                                   DiagnosticLogSink *diagnosticSink) {
    if (outcome == nullptr) {
        return;
    }
    AppSettings compensated = prospective;
    restoreBatchFields(batch, &compensated);
    const AppSettingsSaveResult compensation = SettingsDiagnostics::save(
        persistence, compensated, diagnosticSink, "compensation");
    if (compensation.success) {
        outcome->committedSettings = compensated;
        markUnavailableReceiverCompensated(outcome, batch, unavailable);
        return;
    }

    outcome->globalResult = SettingsApplyGlobalResult{
        SettingsApplyGlobalStatus::PersistenceFailed, compensation};
    markUnavailableReceiverCompensationFailure(outcome, batch, unavailable, compensation);
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
                                                   SettingsChangeDeferrer *deferrer,
                                                   DiagnosticLogSink *diagnosticSink)
    : hotkeys_(hotkeys),
      persistence_(persistence),
      receiver_(receiver),
      deferrer_(deferrer),
      diagnosticSink_(diagnosticSink) {
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

    result.requiresReceiverTimingDecision = receiverSessionActive
        && !result.validChangedReceiverFields.isEmpty();
    return result;
}

SettingsApplyOutcome SettingsApplyCoordinator::execute(const SettingsApplyPlan &plan,
                                                       ReceiverApplyTiming timing) {
    auto outcome = executePlan(plan, timing);
    SettingsDiagnostics::recordApplyOutcome(diagnosticSink_, outcome, "apply", timing);
    return outcome;
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

    const AppSettingsSaveResult persistenceResult = SettingsDiagnostics::save(
        persistence_, prospective, diagnosticSink_, "apply");
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
                compensateUnavailableReceiver(&outcome, batch, prospective, persistence_, unavailable, diagnosticSink_);
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
                compensateUnavailableReceiver(&outcome, batch, prospective, persistence_, unavailable, diagnosticSink_);
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
            compensateUnavailableReceiver(&outcome, batch, prospective, persistence_, unavailable, diagnosticSink_);
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
        const AppSettingsSaveResult compensation = SettingsDiagnostics::save(
            persistence_, compensated, diagnosticSink_, "compensation");
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
    auto outcome = completeDeferredReceiverApplyImpl(batch, currentlyCommitted);
    SettingsDiagnostics::recordApplyOutcome(diagnosticSink_, outcome, "deferred_completion", std::nullopt);
    return outcome;
}

SettingsApplyOutcome SettingsApplyCoordinator::completeDeferredReceiverApplyImpl(
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
        compensateUnavailableReceiver(&outcome, batch, currentlyCommitted, persistence_, unavailable, diagnosticSink_);
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
    const AppSettingsSaveResult compensation = SettingsDiagnostics::save(
        persistence_, compensated, diagnosticSink_, "compensation");
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

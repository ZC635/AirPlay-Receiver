#include "app/SettingsApplyCoordinator.h"

#include "platform/WindowsHotkeyService.h"

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
        } else if (!WindowsHotkeyService::toNativeHotkey(sequence).has_value()) {
            markValidationFailure(fieldResult,
                                  QStringLiteral("Shortcut is not supported as a Windows global hotkey."));
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

#include "app/SettingsDiagnostics.h"
#include "diagnostics/DiagnosticLogSink.h"

namespace SettingsDiagnostics {
AppSettingsSaveResult save(SettingsPersistence *persistence, const AppSettings &settings,
                           DiagnosticLogSink *sink, const char *origin) {
    const auto result = persistence ? persistence->save(settings) : AppSettingsSaveResult{true};
    if (!result.success && sink && sink->isActive()) {
        QString stage = QStringLiteral("unknown");
        if (result.failureStage) {
            switch (*result.failureStage) {
            case AppSettingsSaveStage::Open: stage = QStringLiteral("open"); break;
            case AppSettingsSaveStage::Write: stage = QStringLiteral("write"); break;
            case AppSettingsSaveStage::Commit: stage = QStringLiteral("commit"); break;
            }
        }
        sink->record(makeDiagnosticEvent(DiagnosticSeverity::Warning, QStringLiteral("ui"),
            QStringLiteral("settings_save_failed"),
            {{QStringLiteral("stage"), stage}, {QStringLiteral("origin"), QString::fromLatin1(origin)},
             {QStringLiteral("io_error_code"), QString::number(static_cast<int>(result.fileError))}}, true));
    }
    return result;
}

void recordApplyOutcome(DiagnosticLogSink *sink, const SettingsApplyOutcome &outcome,
                        const char *phase, std::optional<ReceiverApplyTiming> timing) {
    if (!sink || !sink->isActive()) return;
    int applied = 0, deferred = 0, invalid = 0, rolledBack = 0, recoveryFailed = 0;
    for (const auto &field : outcome.fieldResults) {
        switch (field.status) {
        case SettingsFieldStatus::Applied: ++applied; break;
        case SettingsFieldStatus::Deferred: ++deferred; break;
        case SettingsFieldStatus::ValidationFailed: ++invalid; break;
        case SettingsFieldStatus::ApplyFailedRolledBack: ++rolledBack; break;
        case SettingsFieldStatus::RecoveryFailed: ++recoveryFailed; break;
        case SettingsFieldStatus::Unchanged: break;
        }
    }
    const bool failed = outcome.globalResult.has_value() || invalid || rolledBack || recoveryFailed;
    sink->record(makeDiagnosticEvent(failed ? DiagnosticSeverity::Warning : DiagnosticSeverity::Info,
        QStringLiteral("ui"), QStringLiteral("settings_apply_completed"),
        {{QStringLiteral("phase"), QString::fromLatin1(phase)},
         {QStringLiteral("timing"), !timing ? QStringLiteral("deferred")
             : *timing == ReceiverApplyTiming::Immediate ? QStringLiteral("immediate") : QStringLiteral("after_disconnect")},
         {QStringLiteral("applied_count"), QString::number(applied)},
         {QStringLiteral("deferred_count"), QString::number(deferred)},
         {QStringLiteral("validation_failed_count"), QString::number(invalid)},
         {QStringLiteral("rolled_back_count"), QString::number(rolledBack)},
         {QStringLiteral("recovery_failed_count"), QString::number(recoveryFailed)},
         {QStringLiteral("persistence_failed"), outcome.globalResult ? QStringLiteral("yes") : QStringLiteral("no")}}, failed));
}
}

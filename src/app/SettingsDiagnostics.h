#pragma once

#include "app/AppSettingsStore.h"
#include "app/SettingsApplyTypes.h"

class DiagnosticLogSink;

namespace SettingsDiagnostics {
void recordSubmitResult(DiagnosticLogSink *sink, const SettingsSubmitResult &result);
void recordDeferredResult(DiagnosticLogSink *sink, const SettingsDeferredResult &result);
AppSettingsSaveResult save(SettingsPersistence *persistence, const AppSettings &settings,
                           DiagnosticLogSink *sink, const char *origin);
void recordApplyOutcome(DiagnosticLogSink *sink, const SettingsApplyOutcome &outcome,
                        const char *phase, std::optional<ReceiverApplyTiming> timing);
}

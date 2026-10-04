#pragma once

#include "app/AppSettingsStore.h"
#include "app/SettingsApplyTypes.h"

class DiagnosticLogSink;

namespace SettingsDiagnostics {
AppSettingsSaveResult save(SettingsPersistence *persistence, const AppSettings &settings,
                           DiagnosticLogSink *sink, const char *origin);
void recordApplyOutcome(DiagnosticLogSink *sink, const SettingsApplyOutcome &outcome,
                        const char *phase, std::optional<ReceiverApplyTiming> timing);
}

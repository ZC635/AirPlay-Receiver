#pragma once

#include "app/SettingsApplyTypes.h"
#include "backend/ReceiverConfigurationChange.h"
#include "backend/RecordingTypes.h"

#include <QPointer>

class AirPlayReceiver;
class HotkeyService;
class SettingsChangeDeferrer;
class SettingsPersistence;
class DiagnosticLogSink;

class SettingsApplyCoordinator {
public:
    SettingsApplyCoordinator(HotkeyService *hotkeys,
                             SettingsPersistence *persistence,
                             AirPlayReceiver *receiver,
                             SettingsChangeDeferrer *deferrer,
                             DiagnosticLogSink *diagnosticSink = nullptr);

    SettingsApplyPlan plan(const AppSettings &baseline,
                           const AppSettings &candidate,
                           bool receiverSessionActive,
                           RecordingState recordingState) const;

    SettingsApplyOutcome execute(const SettingsApplyPlan &plan,
                                 ReceiverApplyTiming timing);

    SettingsApplyOutcome completeDeferredReceiverApply(
        const ReceiverConfigurationBatchRequest &batch,
        const AppSettings &currentlyCommitted);

private:
    SettingsApplyOutcome executePlan(const SettingsApplyPlan &plan, ReceiverApplyTiming timing);
    SettingsApplyOutcome completeDeferredReceiverApplyImpl(
        const ReceiverConfigurationBatchRequest &batch, const AppSettings &currentlyCommitted);
    QPointer<HotkeyService> hotkeys_;
    SettingsPersistence *persistence_ = nullptr;
    QPointer<AirPlayReceiver> receiver_;
    SettingsChangeDeferrer *deferrer_ = nullptr;
    DiagnosticLogSink *diagnosticSink_ = nullptr;
};

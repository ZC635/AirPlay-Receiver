#pragma once

#include "app/SettingsApplyTypes.h"
#include "backend/RecordingTypes.h"

class AirPlayReceiver;
class HotkeyService;
class SettingsChangeDeferrer;
class SettingsPersistence;

class SettingsApplyCoordinator {
public:
    SettingsApplyCoordinator(HotkeyService *hotkeys,
                             SettingsPersistence *persistence,
                             AirPlayReceiver *receiver,
                             SettingsChangeDeferrer *deferrer);

    SettingsApplyPlan plan(const AppSettings &baseline,
                           const AppSettings &candidate,
                           bool receiverSessionActive,
                           RecordingState recordingState) const;

private:
    HotkeyService *hotkeys_ = nullptr;
    SettingsPersistence *persistence_ = nullptr;
    AirPlayReceiver *receiver_ = nullptr;
    SettingsChangeDeferrer *deferrer_ = nullptr;
};

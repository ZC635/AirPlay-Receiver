#pragma once

#include "backend/ReceiverState.h"
#include "backend/VideoQualitySettings.h"
#include <QString>

enum class ReceiverPolicyAction {
    Noop,
    StoreOnly,
    RestartDiscovery,
    RestartDiscoveryConnected,
    RestartReceiver,
    Reject
};

struct ReceiverNamePolicyResult {
    ReceiverPolicyAction action = ReceiverPolicyAction::Reject;
    QString normalizedName;
    QString rollbackName;
};

struct VideoQualityPolicyResult {
    ReceiverPolicyAction action = ReceiverPolicyAction::Reject;
};

ReceiverNamePolicyResult decideReceiverNameChange(ReceiverState state, const QString &currentName,
                                                  const QString &requestedName);

VideoQualityPolicyResult decideVideoQualityChange(ReceiverState state, const VideoQualitySettings &currentQuality,
                                                   const VideoQualitySettings &requestedQuality);

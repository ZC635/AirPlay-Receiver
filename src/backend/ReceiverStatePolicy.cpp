#include "backend/ReceiverStatePolicy.h"

ReceiverNamePolicyResult decideReceiverNameChange(ReceiverState state, const QString &currentName,
                                                   const QString &requestedName) {
    ReceiverNamePolicyResult result;

    const QString trimmed = requestedName.trimmed();
    if (trimmed.isEmpty()) {
        result.action = ReceiverPolicyAction::Reject;
        return result;
    }

    result.normalizedName = trimmed;
    result.rollbackName = currentName;

    if (currentName == trimmed) {
        result.action = ReceiverPolicyAction::Noop;
        return result;
    }

    if (state == ReceiverState::Idle) {
        result.action = ReceiverPolicyAction::StoreOnly;
        return result;
    }

    if (state == ReceiverState::Connecting || state == ReceiverState::Connected) {
        result.action = ReceiverPolicyAction::RestartDiscoveryConnected;
        return result;
    }

    result.action = ReceiverPolicyAction::RestartDiscovery;
    return result;
}

VideoQualityPolicyResult decideVideoQualityChange(ReceiverState state, const VideoQualitySettings &currentQuality,
                                                   const VideoQualitySettings &requestedQuality) {
    VideoQualityPolicyResult result;

    if (currentQuality == requestedQuality) {
        result.action = ReceiverPolicyAction::Noop;
        return result;
    }

    if (state == ReceiverState::Error || state == ReceiverState::Starting) {
        result.action = ReceiverPolicyAction::Reject;
        return result;
    }

    if (state == ReceiverState::Idle) {
        result.action = ReceiverPolicyAction::StoreOnly;
        return result;
    }

    if (state == ReceiverState::Discoverable) {
        result.action = ReceiverPolicyAction::RestartDiscovery;
        return result;
    }

    result.action = ReceiverPolicyAction::RestartReceiver;
    return result;
}

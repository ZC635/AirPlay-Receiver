#include "backend/ReceiverConfigurationChange.h"

#include "backend/ReceiverStatePolicy.h"

bool applyReceiverNameConfigurationChange(ReceiverState state, const QString &currentName,
                                          const QString &requestedName,
                                          const ReceiverNameChangeOperations &operations) {
    const auto result = decideReceiverNameChange(state, currentName, requestedName);

    switch (result.action) {
    case ReceiverPolicyAction::Reject:
        return false;
    case ReceiverPolicyAction::Noop:
        return true;
    case ReceiverPolicyAction::StoreOnly:
        if (operations.storeName) {
            operations.storeName(result.normalizedName);
        }
        return true;
    case ReceiverPolicyAction::RestartDiscoveryConnected:
        if (operations.storeName) {
            operations.storeName(result.normalizedName);
        }
        if (operations.restartDiscovery && !operations.restartDiscovery()) {
            if (operations.storeName) {
                operations.storeName(result.rollbackName);
            }
            return false;
        }
        return true;
    case ReceiverPolicyAction::RestartDiscovery:
        if (operations.storeName) {
            operations.storeName(result.normalizedName);
        }
        if (operations.restartDiscoveryWithRecovery && !operations.restartDiscoveryWithRecovery(result.rollbackName)) {
            if (operations.storeName) {
                operations.storeName(result.rollbackName);
            }
            if (operations.restartDiscovery && !operations.restartDiscovery() && operations.reportRecoveryFailure) {
                operations.reportRecoveryFailure(QStringLiteral("Failed to recover discovery after receiver rename failure"));
            }
            return false;
        }
        return true;
    case ReceiverPolicyAction::RestartReceiver:
        return true;
    }

    return false;
}

bool applyVideoQualityConfigurationChange(ReceiverState state, const VideoQualitySettings &currentQuality,
                                          const VideoQualitySettings &requestedQuality,
                                          const VideoQualityChangeOperations &operations) {
    const VideoQualitySettings rollbackQuality = currentQuality;
    const auto result = decideVideoQualityChange(state, currentQuality, requestedQuality);

    switch (result.action) {
    case ReceiverPolicyAction::Reject:
        return false;
    case ReceiverPolicyAction::Noop:
        return true;
    case ReceiverPolicyAction::StoreOnly:
        if (operations.storeQuality) {
            operations.storeQuality(requestedQuality);
        }
        return true;
    case ReceiverPolicyAction::RestartDiscovery:
        if (operations.storeQuality) {
            operations.storeQuality(requestedQuality);
        }
        if (operations.updateActiveAdvertisement) {
            operations.updateActiveAdvertisement(requestedQuality);
        }
        if (operations.restartDiscovery && !operations.restartDiscovery()) {
            if (operations.storeQuality) {
                operations.storeQuality(rollbackQuality);
            }
            return false;
        }
        return true;
    case ReceiverPolicyAction::RestartReceiver:
        if (operations.storeQuality) {
            operations.storeQuality(requestedQuality);
        }
        if (operations.restartReceiver && !operations.restartReceiver()) {
            if (operations.storeQuality) {
                operations.storeQuality(rollbackQuality);
            }
            return false;
        }
        return true;
    }

    return false;
}

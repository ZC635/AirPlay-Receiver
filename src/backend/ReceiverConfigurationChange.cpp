#include "backend/ReceiverConfigurationChange.h"

#include "backend/ReceiverStatePolicy.h"

#include <utility>

namespace {
bool hasVideoQualityChange(const ReceiverConfigurationBatchRequest &request) {
    return request.resolutionChanged || request.frameRateChanged;
}

VideoQualitySettings mergedRuntimeVideoQuality(const ReceiverConfigurationBatchRequest &request) {
    VideoQualitySettings result = request.rollbackVideoQuality;
    if (request.resolutionChanged) {
        result.resolution = request.requestedVideoQuality.resolution;
    }
    if (request.frameRateChanged) {
        result.frameRate = request.requestedVideoQuality.frameRate;
    }
    return result;
}

ReceiverConfigurationBatchResult failedBeforeRestart(const ReceiverConfigurationBatchRequest &request,
                                                      QString error) {
    ReceiverConfigurationBatchResult result;
    result.status = ReceiverConfigurationBatchStatus::ApplyFailedRolledBack;
    result.applyError = std::move(error);
    result.knownRuntimeReceiverName = request.rollbackReceiverName;
    result.knownRuntimeVideoQuality = request.rollbackVideoQuality;
    return result;
}
} // namespace

ReceiverConfigurationBatchResult applyReceiverConfigurationBatch(
    const ReceiverConfigurationBatchRequest &request,
    const ReceiverConfigurationBatchOperations &operations) {
    const bool qualityChanged = hasVideoQualityChange(request);
    const bool anyChange = request.receiverNameChanged || qualityChanged;

    ReceiverConfigurationBatchResult result;
    result.knownRuntimeReceiverName = request.receiverNameChanged
        ? request.requestedReceiverName : request.rollbackReceiverName;
    result.knownRuntimeVideoQuality = mergedRuntimeVideoQuality(request);
    if (!anyChange) {
        return result;
    }

    if (request.receiverNameChanged && !operations.storeReceiverName) {
        return failedBeforeRestart(request, QStringLiteral("Missing receiver name storage operation"));
    }
    if (qualityChanged && !operations.storeVideoQuality) {
        return failedBeforeRestart(request, QStringLiteral("Missing video quality storage operation"));
    }
    if (!operations.restartWithRequestedConfiguration) {
        return failedBeforeRestart(request, QStringLiteral("Missing requested configuration restart operation"));
    }

    if (request.receiverNameChanged) {
        operations.storeReceiverName(request.requestedReceiverName);
    }
    if (qualityChanged) {
        operations.storeVideoQuality(request.requestedVideoQuality);
    }

    const ReceiverOperationResult applied = operations.restartWithRequestedConfiguration();
    if (applied.success) {
        return result;
    }

    result.applyError = applied.error.isEmpty()
        ? QStringLiteral("Requested configuration restart failed") : applied.error;
    if (request.receiverNameChanged) {
        operations.storeReceiverName(request.rollbackReceiverName);
    }
    if (qualityChanged) {
        operations.storeVideoQuality(request.rollbackVideoQuality);
    }

    result.knownRuntimeReceiverName = request.rollbackReceiverName;
    result.knownRuntimeVideoQuality = request.rollbackVideoQuality;
    if (!operations.restartWithRollbackConfiguration) {
        result.status = ReceiverConfigurationBatchStatus::RecoveryFailed;
        result.recoveryError = QStringLiteral("Missing rollback configuration restart operation");
        return result;
    }

    const ReceiverOperationResult recovered = operations.restartWithRollbackConfiguration();
    if (recovered.success) {
        result.status = ReceiverConfigurationBatchStatus::ApplyFailedRolledBack;
        return result;
    }

    result.status = ReceiverConfigurationBatchStatus::RecoveryFailed;
    result.recoveryError = recovered.error.isEmpty()
        ? QStringLiteral("Rollback configuration restart failed") : recovered.error;
    return result;
}

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

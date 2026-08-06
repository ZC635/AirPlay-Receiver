#pragma once

#include "backend/ReceiverState.h"
#include "backend/VideoQualitySettings.h"

#include <QString>

#include <functional>

struct ReceiverConfigurationBatchRequest {
    bool receiverNameChanged = false;
    bool resolutionChanged = false;
    bool frameRateChanged = false;
    QString requestedReceiverName;
    QString rollbackReceiverName;
    VideoQualitySettings requestedVideoQuality;
    VideoQualitySettings rollbackVideoQuality;
};

enum class ReceiverConfigurationBatchStatus {
    Applied,
    Deferred,
    ApplyFailedRolledBack,
    RecoveryFailed,
};

struct ReceiverConfigurationBatchResult {
    ReceiverConfigurationBatchStatus status = ReceiverConfigurationBatchStatus::Applied;
    QString applyError;
    QString recoveryError;
    QString knownRuntimeReceiverName;
    VideoQualitySettings knownRuntimeVideoQuality;
};

struct ReceiverOperationResult {
    bool success = false;
    QString error;
};

struct ReceiverConfigurationBatchOperations {
    std::function<void(const QString &)> storeReceiverName;
    std::function<void(const VideoQualitySettings &)> storeVideoQuality;
    std::function<ReceiverOperationResult()> restartWithRequestedConfiguration;
    std::function<ReceiverOperationResult()> restartWithRollbackConfiguration;
};

ReceiverConfigurationBatchResult applyReceiverConfigurationBatch(
    const ReceiverConfigurationBatchRequest &request,
    const ReceiverConfigurationBatchOperations &operations);

struct ReceiverNameChangeOperations {
    std::function<void(const QString &)> storeName;
    std::function<bool()> restartDiscovery;
    std::function<bool(const QString &)> restartDiscoveryWithRecovery;
    std::function<void(const QString &)> reportRecoveryFailure;
};

struct VideoQualityChangeOperations {
    std::function<void(const VideoQualitySettings &)> storeQuality;
    std::function<void(const VideoQualitySettings &)> updateActiveAdvertisement;
    std::function<bool()> restartDiscovery;
    std::function<bool()> restartReceiver;
};

bool applyReceiverNameConfigurationChange(ReceiverState state, const QString &currentName,
                                          const QString &requestedName,
                                          const ReceiverNameChangeOperations &operations);

bool applyVideoQualityConfigurationChange(ReceiverState state, const VideoQualitySettings &currentQuality,
                                          const VideoQualitySettings &requestedQuality,
                                          const VideoQualityChangeOperations &operations);

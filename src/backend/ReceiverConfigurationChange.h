#pragma once

#include "backend/ReceiverState.h"
#include "backend/VideoQualitySettings.h"

#include <QString>

#include <functional>

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

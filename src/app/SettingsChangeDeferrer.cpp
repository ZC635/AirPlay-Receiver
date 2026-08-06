#include "app/SettingsChangeDeferrer.h"

SettingsChangeDeferrer::SettingsChangeDeferrer(QObject *parent)
    : QObject(parent) {}

void SettingsChangeDeferrer::deferReceiverConfiguration(
    ReceiverConfigurationBatchRequest batch,
    bool waitForSessionEnd,
    bool waitForRecordingIdle) {
    pendingReceiverConfiguration_ = std::move(batch);
    receiverConfigurationWaitsForSessionEnd_ = waitForSessionEnd;
    receiverConfigurationWaitsForRecordingIdle_ = waitForRecordingIdle;
    emitReceiverConfigurationIfReady();
}

void SettingsChangeDeferrer::cancelPendingReceiverConfiguration() {
    pendingReceiverConfiguration_.reset();
    receiverConfigurationWaitsForSessionEnd_ = false;
    receiverConfigurationWaitsForRecordingIdle_ = false;
}

bool SettingsChangeDeferrer::hasPendingReceiverConfiguration() const {
    return pendingReceiverConfiguration_.has_value();
}

bool SettingsChangeDeferrer::isReceiverNamePending(QString name) const {
    return pendingReceiverName_.has_value() && *pendingReceiverName_ == name;
}

bool SettingsChangeDeferrer::isVideoQualityPending(VideoQualitySettings quality) const {
    return pendingVideoQuality_.has_value() && *pendingVideoQuality_ == quality;
}

void SettingsChangeDeferrer::receiverNameChanged(QString requestedName, QString activeName) {
    if (requestedName == activeName) {
        pendingReceiverName_.reset();
        receiverNameWaitsForRecordingIdle_ = false;
        return;
    }

    if (pendingReceiverName_.has_value() && requestedName == *pendingReceiverName_) {
        return;
    }

    pendingReceiverName_ = requestedName;
    receiverNameWaitsForRecordingIdle_ = false;
}

void SettingsChangeDeferrer::videoQualityChanged(VideoQualitySettings requestedQuality, VideoQualitySettings activeQuality) {
    if (requestedQuality == activeQuality) {
        pendingVideoQuality_.reset();
        videoQualityWaitsForRecordingIdle_ = false;
        return;
    }

    if (pendingVideoQuality_.has_value() && requestedQuality == *pendingVideoQuality_) {
        return;
    }

    pendingVideoQuality_ = requestedQuality;
    videoQualityWaitsForRecordingIdle_ = false;
}

void SettingsChangeDeferrer::recordingStateChanged(RecordingState previous,
                                                   RecordingState current) {
    if (current == RecordingState::Idle && receiverConfigurationWaitsForRecordingIdle_) {
        receiverConfigurationWaitsForRecordingIdle_ = false;
        emitReceiverConfigurationIfReady();
    }

    if (previous != RecordingState::Finalizing || current != RecordingState::Idle) {
        return;
    }

    if (receiverNameWaitsForRecordingIdle_ && pendingReceiverName_.has_value()) {
        const QString readyName = *pendingReceiverName_;
        emit receiverNameReady(readyName);
        if (receiverNameWaitsForRecordingIdle_ && pendingReceiverName_.has_value() &&
            *pendingReceiverName_ == readyName) {
            receiverNameWaitsForRecordingIdle_ = false;
        }
    }
    if (videoQualityWaitsForRecordingIdle_ && pendingVideoQuality_.has_value()) {
        const VideoQualitySettings readyQuality = *pendingVideoQuality_;
        emit videoQualityReady(readyQuality);
        if (videoQualityWaitsForRecordingIdle_ && pendingVideoQuality_.has_value() &&
            *pendingVideoQuality_ == readyQuality) {
            videoQualityWaitsForRecordingIdle_ = false;
        }
    }
}

void SettingsChangeDeferrer::deferReceiverNameUntilRecordingIdle(QString name) {
    pendingReceiverName_ = std::move(name);
    receiverNameWaitsForRecordingIdle_ = true;
}

void SettingsChangeDeferrer::deferVideoQualityUntilRecordingIdle(
    VideoQualitySettings quality) {
    pendingVideoQuality_ = quality;
    videoQualityWaitsForRecordingIdle_ = true;
}

void SettingsChangeDeferrer::receiverSessionChanged(bool wasSessionActive, bool sessionActive) {
    if (!sessionActive && receiverConfigurationWaitsForSessionEnd_) {
        receiverConfigurationWaitsForSessionEnd_ = false;
        emitReceiverConfigurationIfReady();
    }

    if (wasSessionActive && !sessionActive && pendingReceiverName_.has_value() &&
        !receiverNameWaitsForRecordingIdle_) {
        emit receiverNameReady(*pendingReceiverName_);
    }

    if (!sessionActive && pendingVideoQuality_.has_value() &&
        !videoQualityWaitsForRecordingIdle_) {
        emit videoQualityReady(*pendingVideoQuality_);
    }
}

void SettingsChangeDeferrer::markReceiverNameApplied(QString name) {
    if (pendingReceiverName_.has_value() && *pendingReceiverName_ == name) {
        pendingReceiverName_.reset();
        receiverNameWaitsForRecordingIdle_ = false;
    }
}

void SettingsChangeDeferrer::markVideoQualityApplied(VideoQualitySettings quality) {
    if (pendingVideoQuality_.has_value() && *pendingVideoQuality_ == quality) {
        pendingVideoQuality_.reset();
        videoQualityWaitsForRecordingIdle_ = false;
    }
}

void SettingsChangeDeferrer::emitReceiverConfigurationIfReady() {
    if (!pendingReceiverConfiguration_.has_value() || receiverConfigurationWaitsForSessionEnd_ ||
        receiverConfigurationWaitsForRecordingIdle_) {
        return;
    }

    ReceiverConfigurationBatchRequest ready = std::move(*pendingReceiverConfiguration_);
    pendingReceiverConfiguration_.reset();
    receiverConfigurationWaitsForSessionEnd_ = false;
    receiverConfigurationWaitsForRecordingIdle_ = false;
    emit receiverConfigurationReady(std::move(ready));
}

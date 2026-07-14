#include "app/SettingsChangeDeferrer.h"

SettingsChangeDeferrer::SettingsChangeDeferrer(QObject *parent)
    : QObject(parent) {}

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
    if (previous != RecordingState::Finalizing || current != RecordingState::Idle) {
        return;
    }

    if (receiverNameWaitsForRecordingIdle_ && pendingReceiverName_.has_value()) {
        receiverNameWaitsForRecordingIdle_ = false;
        emit receiverNameReady(*pendingReceiverName_);
    }
    if (videoQualityWaitsForRecordingIdle_ && pendingVideoQuality_.has_value()) {
        videoQualityWaitsForRecordingIdle_ = false;
        emit videoQualityReady(*pendingVideoQuality_);
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

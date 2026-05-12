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
        return;
    }

    if (pendingReceiverName_.has_value() && requestedName == *pendingReceiverName_) {
        return;
    }

    pendingReceiverName_ = requestedName;
}

void SettingsChangeDeferrer::videoQualityChanged(VideoQualitySettings requestedQuality, VideoQualitySettings activeQuality) {
    if (requestedQuality == activeQuality) {
        pendingVideoQuality_.reset();
        return;
    }

    if (pendingVideoQuality_.has_value() && requestedQuality == *pendingVideoQuality_) {
        return;
    }

    pendingVideoQuality_ = requestedQuality;
}

void SettingsChangeDeferrer::receiverSessionChanged(bool wasSessionActive, bool sessionActive) {
    if (wasSessionActive && !sessionActive && pendingReceiverName_.has_value()) {
        emit receiverNameReady(*pendingReceiverName_);
    }

    if (!sessionActive && pendingVideoQuality_.has_value()) {
        emit videoQualityReady(*pendingVideoQuality_);
    }
}

void SettingsChangeDeferrer::markReceiverNameApplied(QString name) {
    if (pendingReceiverName_.has_value() && *pendingReceiverName_ == name) {
        pendingReceiverName_.reset();
    }
}

void SettingsChangeDeferrer::markVideoQualityApplied(VideoQualitySettings quality) {
    if (pendingVideoQuality_.has_value() && *pendingVideoQuality_ == quality) {
        pendingVideoQuality_.reset();
    }
}

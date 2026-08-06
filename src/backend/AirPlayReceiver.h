#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QWidget>
#include <functional>

#include "backend/ReceiverState.h"
#include "backend/ReceiverConfigurationChange.h"
#include "backend/RecordingTypes.h"
#include "backend/VideoQualitySettings.h"

class AirPlayReceiver : public QObject {
    Q_OBJECT

public:
    using FrameCallback = std::function<void(QImage)>;
    using QObject::QObject;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void setVolume(double volume) = 0;
    virtual ReceiverState state() const = 0;
    virtual QString receiverName() const = 0;
    virtual VideoQualitySettings videoQuality() const = 0;
    virtual ReceiverConfigurationBatchResult applyConfigurationBatch(
        const ReceiverConfigurationBatchRequest &request) = 0;
    virtual bool applyReceiverName(const QString &name) {
        const QString normalizedName = name.trimmed();
        if (normalizedName.isEmpty()) {
            return false;
        }
        const VideoQualitySettings currentQuality = videoQuality();
        ReceiverConfigurationBatchRequest request;
        request.receiverNameChanged = normalizedName != receiverName();
        request.requestedReceiverName = normalizedName;
        request.rollbackReceiverName = receiverName();
        request.requestedVideoQuality = currentQuality;
        request.rollbackVideoQuality = currentQuality;
        return applyConfigurationBatch(request).status == ReceiverConfigurationBatchStatus::Applied;
    }
    virtual void setVideoSurface(WId id) { Q_UNUSED(id); }
    virtual void setVideoFrameCallback(FrameCallback callback) { Q_UNUSED(callback); }
    virtual void setVideoFitMode(bool enabled) { Q_UNUSED(enabled); }
    virtual bool applyVideoQuality(const VideoQualitySettings &quality) {
        const VideoQualitySettings currentQuality = videoQuality();
        ReceiverConfigurationBatchRequest request;
        request.resolutionChanged = quality.resolution != currentQuality.resolution;
        request.frameRateChanged = quality.frameRate != currentQuality.frameRate;
        request.requestedReceiverName = receiverName();
        request.rollbackReceiverName = receiverName();
        request.requestedVideoQuality = quality;
        request.rollbackVideoQuality = currentQuality;
        return applyConfigurationBatch(request).status == ReceiverConfigurationBatchStatus::Applied;
    }
    virtual bool recordingAvailable() const = 0;
    virtual RecordingState recordingState() const = 0;
    virtual RecordingStartResult startRecording(const RecordingOptions &options) = 0;
    virtual void stopRecording() = 0;
    virtual void discardRecording() = 0;
    virtual void acknowledgeRecordingResult() = 0;

signals:
    void stateChanged(ReceiverState state);
    void errorChanged(QString error);
    void volumeChanged(double volume);
    void videoSizeChanged(int width, int height);
    void recordingAvailabilityChanged(bool available);
    void recordingStateChanged(RecordingState state);
    void recordingFinished(RecordingResult result);
    void recordingFailed(QString error);
};

#pragma once

#include "backend/AirPlayReceiver.h"
#include "backend/ReceiverConfigurationChange.h"

#include <QImage>
#include <QStringList>
#include <QVector>
#include <functional>

class FakeAirPlayReceiver : public AirPlayReceiver {
    Q_OBJECT

public:
    using AirPlayReceiver::AirPlayReceiver;

    void start() override {
        ++startCount;
        setState(ReceiverState::Discoverable);
    }

    void stop() override {
        ++stopCount;
        setState(ReceiverState::Idle);
    }

    void setVolume(double volume) override { m_volume = volume; }

    void setVideoSurface(WId id) override {
        m_videoSurfaceId = id;
        AirPlayReceiver::setVideoSurface(id);
    }

    void setVideoFrameCallback(FrameCallback callback) override {
        AirPlayReceiver::setVideoFrameCallback(callback);
        m_frameCallback = std::move(callback);
    }

    FrameCallback frameCallback() const { return m_frameCallback; }

    void setVideoFitMode(bool enabled) override { m_videoFitMode = enabled; }

    bool lastVideoFitMode() const { return m_videoFitMode; }

    VideoQualitySettings videoQuality() const override { return lastAppliedVideoQuality; }

    ReceiverConfigurationBatchResult applyConfigurationBatch(
        const ReceiverConfigurationBatchRequest &request) override {
        ++configurationBatchCount;
        configurationBatchRequests.append(request);
        const bool qualityChanged = request.resolutionChanged || request.frameRateChanged;

        auto rejected = [&](const QString &error) {
            ReceiverConfigurationBatchResult result;
            result.status = ReceiverConfigurationBatchStatus::ApplyFailedRolledBack;
            result.applyError = error;
            result.knownRuntimeReceiverName = request.rollbackReceiverName;
            result.knownRuntimeVideoQuality = request.rollbackVideoQuality;
            return result;
        };
        if (request.receiverNameChanged && rejectedReceiverNames.contains(request.requestedReceiverName)) {
            return rejected(QStringLiteral("Requested receiver name is rejected"));
        }
        if (qualityChanged && rejectedVideoQualities.contains(request.requestedVideoQuality)) {
            return rejected(QStringLiteral("Requested video quality is rejected"));
        }
        if (qualityChanged && (m_state == ReceiverState::Starting || m_state == ReceiverState::Error)) {
            return rejected(QStringLiteral("Cannot apply video quality while receiver is starting or in error state"));
        }

        const auto storeName = [&](const QString &name) {
            m_receiverName = name;
            appliedReceiverNames.append(name);
        };
        const auto storeQuality = [&](const VideoQualitySettings &quality) {
            lastAppliedVideoQuality = quality;
        };
        if (!request.receiverNameChanged && !qualityChanged) {
            return applyReceiverConfigurationBatch(request, {});
        }
        if (m_state == ReceiverState::Idle) {
            if (request.receiverNameChanged) {
                storeName(request.requestedReceiverName);
            }
            if (qualityChanged) {
                storeQuality(request.requestedVideoQuality);
            }
            ReceiverConfigurationBatchResult result;
            result.knownRuntimeReceiverName = request.receiverNameChanged
                ? request.requestedReceiverName : request.rollbackReceiverName;
            result.knownRuntimeVideoQuality = request.rollbackVideoQuality;
            if (request.resolutionChanged) {
                result.knownRuntimeVideoQuality.resolution = request.requestedVideoQuality.resolution;
            }
            if (request.frameRateChanged) {
                result.knownRuntimeVideoQuality.frameRate = request.requestedVideoQuality.frameRate;
            }
            return result;
        }

        const bool restartReceiver = (request.receiverNameChanged || qualityChanged)
            && (m_state == ReceiverState::Connecting || m_state == ReceiverState::Connected);
        const auto restart = [this, restartReceiver](const QString &error) {
            ++configurationRestartCount;
            if (!error.isEmpty()) {
                return ReceiverOperationResult{false, error};
            }
            if (restartReceiver) {
                stop();
                start();
            } else {
                ++broadcastRestartCount;
            }
            return ReceiverOperationResult{true, {}};
        };

        ReceiverConfigurationBatchOperations operations;
        operations.storeReceiverName = storeName;
        operations.storeVideoQuality = storeQuality;
        operations.restartWithRequestedConfiguration = [&] { return restart(requestedConfigurationRestartError); };
        operations.restartWithRollbackConfiguration = [&] { return restart(rollbackConfigurationRestartError); };
        return applyReceiverConfigurationBatch(request, operations);
    }

    bool recordingAvailable() const override { return m_recordingAvailable; }

    RecordingState recordingState() const override { return m_recordingState; }

    RecordingStartResult startRecording(const RecordingOptions &options) override {
        if (!m_recordingAvailable) {
            return {false, QStringLiteral("Recording is not available")};
        }
        if (m_recordingState != RecordingState::Idle) {
            return {false, QStringLiteral("Recording is already active")};
        }

        lastRecordingOptions = options;
        ++startRecordingCount;
        setRecordingState(RecordingState::Recording);
        return {true, {}};
    }

    void stopRecording() override {
        if (m_recordingState != RecordingState::Recording) {
            return;
        }

        ++stopRecordingCount;
        setRecordingState(RecordingState::Finalizing);
    }

    void discardRecording() override {
        if (m_recordingState != RecordingState::Recording && m_recordingState != RecordingState::Finalizing) {
            return;
        }

        ++discardRecordingCount;
        setRecordingState(RecordingState::Idle);
    }

    void acknowledgeRecordingResult() override {
        ++acknowledgeRecordingResultCount;
    }

    void setRecordingAvailableForTest(bool available) {
        if (m_recordingAvailable == available) {
            return;
        }

        m_recordingAvailable = available;
        emit recordingAvailabilityChanged(m_recordingAvailable);
    }

    void completeRecordingForTest(const RecordingResult &result) {
        if (m_recordingState != RecordingState::Finalizing) {
            return;
        }

        setRecordingState(RecordingState::Idle);
        emit recordingFinished(result);
    }

    void failRecordingForTest(const QString &error) {
        if (m_recordingState != RecordingState::Recording && m_recordingState != RecordingState::Finalizing) {
            return;
        }

        setRecordingState(RecordingState::Idle);
        emit recordingFailed(error);
    }

    VideoQualitySettings lastAppliedVideoQuality;
    QVector<VideoQualitySettings> rejectedVideoQualities;
    RecordingOptions lastRecordingOptions;

    WId videoSurfaceId() const { return m_videoSurfaceId; }

    ReceiverState state() const override { return m_state; }

    QString receiverName() const override { return m_receiverName; }

    double volume() const { return m_volume; }

    void forceState(ReceiverState state) { setState(state); }

    QStringList appliedReceiverNames;
    QStringList rejectedReceiverNames;
    QVector<ReceiverConfigurationBatchRequest> configurationBatchRequests;
    QString requestedConfigurationRestartError;
    QString rollbackConfigurationRestartError;
    int broadcastRestartCount = 0;
    int configurationBatchCount = 0;
    int configurationRestartCount = 0;
    int startCount = 0;
    int stopCount = 0;
    int startRecordingCount = 0;
    int stopRecordingCount = 0;
    int discardRecordingCount = 0;
    int acknowledgeRecordingResultCount = 0;

    void emitVideoSize(int width, int height) {
        emit videoSizeChanged(width, height);
    }

private:
    void setRecordingState(RecordingState state) {
        if (m_recordingState == state) {
            return;
        }

        m_recordingState = state;
        emit recordingStateChanged(m_recordingState);
    }

    void setState(ReceiverState state) {
        if (m_state == state) {
            return;
        }

        m_state = state;
        emit stateChanged(m_state);
    }

    ReceiverState m_state = ReceiverState::Idle;
    QString m_receiverName = "AirPlay Receiver";
    double m_volume = 1.0;
    WId m_videoSurfaceId = 0;
    FrameCallback m_frameCallback;
    bool m_videoFitMode = false;
    bool m_recordingAvailable = false;
    RecordingState m_recordingState = RecordingState::Idle;
};

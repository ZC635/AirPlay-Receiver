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

    bool applyVideoQuality(const VideoQualitySettings &quality) override {
        VideoQualityChangeOperations operations;
        operations.storeQuality = [&](const VideoQualitySettings &requestedQuality) {
            lastAppliedVideoQuality = requestedQuality;
        };
        operations.restartReceiver = [&] {
            stop();
            start();
            return true;
        };
        if (lastAppliedVideoQuality != quality && rejectedVideoQualities.contains(quality)) {
            return false;
        }
        return applyVideoQualityConfigurationChange(m_state, lastAppliedVideoQuality, quality, operations);
    }

    VideoQualitySettings lastAppliedVideoQuality;
    QVector<VideoQualitySettings> rejectedVideoQualities;

    WId videoSurfaceId() const { return m_videoSurfaceId; }

    ReceiverState state() const override { return m_state; }

    QString receiverName() const override { return m_receiverName; }

    bool applyReceiverName(const QString &name) override {
        if (rejectedReceiverNames.contains(name)) {
            return false;
        }
        ReceiverNameChangeOperations operations;
        operations.storeName = [&](const QString &requestedName) {
            m_receiverName = requestedName;
            appliedReceiverNames.append(requestedName);
        };
        operations.restartDiscovery = [&] {
            if (m_state == ReceiverState::Connecting || m_state == ReceiverState::Connected) {
                stop();
                start();
            } else {
                ++broadcastRestartCount;
            }
            return true;
        };
        operations.restartDiscoveryWithRecovery = [&](const QString &) {
            ++broadcastRestartCount;
            return true;
        };
        return applyReceiverNameConfigurationChange(m_state, m_receiverName, name, operations);
    }

    double volume() const { return m_volume; }

    void forceState(ReceiverState state) { setState(state); }

    QStringList appliedReceiverNames;
    QStringList rejectedReceiverNames;
    int broadcastRestartCount = 0;
    int startCount = 0;
    int stopCount = 0;

    void emitVideoSize(int width, int height) {
        emit videoSizeChanged(width, height);
    }

private:
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
};

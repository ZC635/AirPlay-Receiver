#pragma once

#include "backend/AirPlayReceiver.h"

#include <QStringList>

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

    WId videoSurfaceId() const { return m_videoSurfaceId; }

    ReceiverState state() const override { return m_state; }

    QString receiverName() const override { return m_receiverName; }

    bool applyReceiverName(const QString &name) override {
        if (rejectedReceiverNames.contains(name)) {
            return false;
        }
        if (m_receiverName == name) {
            return true;
        }

        if (m_state == ReceiverState::Connecting || m_state == ReceiverState::Connected) {
            stop();
            m_receiverName = name;
            appliedReceiverNames.append(name);
            start();
            return true;
        }

        if (m_state != ReceiverState::Idle) {
            ++broadcastRestartCount;
        }

        m_receiverName = name;
        appliedReceiverNames.append(name);
        return true;
    }

    double volume() const { return m_volume; }

    void forceState(ReceiverState state) { setState(state); }

    QStringList appliedReceiverNames;
    QStringList rejectedReceiverNames;
    int broadcastRestartCount = 0;
    int startCount = 0;
    int stopCount = 0;

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
};

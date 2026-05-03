#pragma once

#include "backend/AirPlayReceiver.h"

class FakeAirPlayReceiver : public AirPlayReceiver {
    Q_OBJECT

public:
    using AirPlayReceiver::AirPlayReceiver;

    void start() override { setState(ReceiverState::Discoverable); }

    void stop() override { setState(ReceiverState::Idle); }

    void setVolume(double volume) override { m_volume = volume; }

    ReceiverState state() const override { return m_state; }

    double volume() const { return m_volume; }

private:
    void setState(ReceiverState state) {
        if (m_state == state) {
            return;
        }

        m_state = state;
        emit stateChanged(m_state);
    }

    ReceiverState m_state = ReceiverState::Idle;
    double m_volume = 1.0;
};

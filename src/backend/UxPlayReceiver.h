#pragma once

#include <QString>

#include "backend/AirPlayReceiver.h"

struct UxPlayReceiverConfig {
    QString serverName = "AirPlay Receiver";
    QString videoSink = "autovideosink";
    QString audioSink = "autoaudiosink";
    int basePort = 0;
};

class UxPlayReceiver : public AirPlayReceiver {
    Q_OBJECT

public:
    explicit UxPlayReceiver(UxPlayReceiverConfig config = {}, QObject *parent = nullptr);

    void start() override;
    void stop() override;
    void setVolume(double volume) override;
    ReceiverState state() const override;

private:
    void setState(ReceiverState state);
    void setError(QString error);

    UxPlayReceiverConfig m_config;
    ReceiverState m_state = ReceiverState::Idle;
    QString m_error;
    double m_volume = 1.0;
};

#pragma once

#include <QObject>
#include <QString>

#include "backend/ReceiverState.h"

class AirPlayReceiver : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void setVolume(double volume) = 0;
    virtual ReceiverState state() const = 0;

signals:
    void stateChanged(ReceiverState state);
    void errorChanged(QString error);
};

#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QWidget>
#include <functional>

#include "backend/ReceiverState.h"
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
    virtual bool applyReceiverName(const QString &name) = 0;
    virtual void setVideoSurface(WId id) { Q_UNUSED(id); }
    virtual void setVideoFrameCallback(FrameCallback callback) { Q_UNUSED(callback); }
    virtual void setVideoFitMode(bool enabled) { Q_UNUSED(enabled); }
    virtual bool applyVideoQuality(const VideoQualitySettings &quality) { Q_UNUSED(quality); return true; }

signals:
    void stateChanged(ReceiverState state);
    void errorChanged(QString error);
    void videoSizeChanged(int width, int height);
};

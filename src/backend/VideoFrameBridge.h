#pragma once

#include <QObject>
#include <QImage>

#include <gst/gst.h>

#include "backend/GstAppSinkFrameSource.h"

class VideoFrameBridge : public QObject {
    Q_OBJECT

public:
    explicit VideoFrameBridge(GstElement *appsink, QObject *parent = nullptr);
    // Non-owning: caller must ensure source outlives this bridge.
    explicit VideoFrameBridge(AppSinkFrameSource *source, QObject *parent = nullptr);
    ~VideoFrameBridge() override;
    void start();
    void processFrame();

signals:
    void frameReady(QImage frame);

private:
    AppSinkFrameSource *m_source = nullptr;
    bool m_ownsSource = false;
    bool m_started = false;
};

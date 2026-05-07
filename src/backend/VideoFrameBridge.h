#pragma once

#include <QObject>
#include <QImage>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

class VideoFrameBridge : public QObject {
    Q_OBJECT

public:
    explicit VideoFrameBridge(GstElement *appsink, QObject *parent = nullptr);
    ~VideoFrameBridge() override;
    void start();

signals:
    void frameReady(QImage frame);

private:
    static GstFlowReturn onNewSample(GstAppSink *appsink, gpointer userData);
    GstElement *appsink_ = nullptr;
};

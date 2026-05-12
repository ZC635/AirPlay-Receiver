#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

struct VideoFrameSample {
    QByteArray bytes;
    int width = 0;
    int height = 0;
    int bytesPerLine = 0;
    QString format;
};

class AppSinkFrameSource {
public:
    virtual ~AppSinkFrameSource() = default;
    virtual std::optional<VideoFrameSample> pullSample() = 0;
    virtual void start() {}
};

class GstAppSinkFrameSource : public AppSinkFrameSource {
public:
    explicit GstAppSinkFrameSource(GstElement *appsink);
    ~GstAppSinkFrameSource() override;

    std::optional<VideoFrameSample> pullSample() override;
    void start() override;

private:
    GstElement *m_appsink = nullptr;
    bool m_started = false;
};

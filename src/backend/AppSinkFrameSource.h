#pragma once

#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>

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
    virtual void setFrameAvailableCallback(std::function<void()> callback) = 0;
    virtual void start() {}
};

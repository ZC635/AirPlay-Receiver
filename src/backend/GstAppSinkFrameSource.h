#pragma once

#include <QByteArray>
#include <QString>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
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
    virtual void setFrameAvailableCallback(std::function<void()> callback) = 0;
    virtual void start() {}
};

class GstAppSinkFrameSource : public AppSinkFrameSource {
public:
    explicit GstAppSinkFrameSource(GstElement *appsink);
    ~GstAppSinkFrameSource() override;

    std::optional<VideoFrameSample> pullSample() override;
    void setFrameAvailableCallback(std::function<void()> callback) override;
    void start() override;

private:
    struct CallbackState {
        std::mutex mutex;
        std::condition_variable idle;
        std::function<void()> frameAvailableCallback;
        int callbacksInFlight = 0;
        int callbacksBlockedInClear = 0;
    };

    static GstFlowReturn onNewSample(GstAppSink *appsink, gpointer userData);

    GstElement *m_appsink = nullptr;
    std::shared_ptr<CallbackState> m_callbackState;
    std::shared_ptr<CallbackState> *m_signalCallbackState = nullptr;
    gulong m_newSampleHandlerId = 0;
    bool m_started = false;
};

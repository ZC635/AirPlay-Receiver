#pragma once

#include "backend/AppSinkFrameSource.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

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

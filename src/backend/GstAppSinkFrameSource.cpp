#include "backend/GstAppSinkFrameSource.h"

#include <gst/video/gstvideometa.h>

#include <unordered_map>

namespace {
thread_local std::unordered_map<const void *, int> activeCallbackDepthByState;
thread_local std::unordered_map<const void *, int> blockedClearDepthByState;
}

GstAppSinkFrameSource::GstAppSinkFrameSource(GstElement *appsink)
    : m_appsink(appsink ? GST_ELEMENT(gst_object_ref(appsink)) : nullptr),
      m_callbackState(std::make_shared<CallbackState>()) {}

GstAppSinkFrameSource::~GstAppSinkFrameSource() {
    if (m_appsink && m_newSampleHandlerId != 0) {
        g_signal_handler_disconnect(m_appsink, m_newSampleHandlerId);
        m_newSampleHandlerId = 0;
        m_signalCallbackState = nullptr;
    }
    setFrameAvailableCallback({});
    if (m_appsink) {
        gst_object_unref(m_appsink);
    }
}

void GstAppSinkFrameSource::setFrameAvailableCallback(std::function<void()> callback) {
    auto state = m_callbackState;
    std::unique_lock lock(state->mutex);
    state->frameAvailableCallback = std::move(callback);
    if (!state->frameAvailableCallback) {
        const int currentThreadDepth = activeCallbackDepthByState[state.get()];
        if (currentThreadDepth == 0) {
            state->idle.wait(lock, [&state] { return state->callbacksInFlight == 0; });
        } else {
            state->callbacksBlockedInClear++;
            blockedClearDepthByState[state.get()]++;
            state->idle.wait(lock, [&state, currentThreadDepth] {
                const int currentThreadBlockedDepth = blockedClearDepthByState[state.get()];
                const int independentlyProgressingCallbacks =
                    state->callbacksInFlight - state->callbacksBlockedInClear -
                    (currentThreadDepth - currentThreadBlockedDepth);
                return independentlyProgressingCallbacks == 0;
            });
            if (--blockedClearDepthByState[state.get()] == 0) {
                blockedClearDepthByState.erase(state.get());
            }
            state->callbacksBlockedInClear--;
            state->idle.notify_all();
        }
    }
}

void GstAppSinkFrameSource::start() {
    if (m_started || !m_appsink) return;
    m_started = true;
    gst_app_sink_set_emit_signals(GST_APP_SINK(m_appsink), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(m_appsink), 2);
    gst_app_sink_set_drop(GST_APP_SINK(m_appsink), TRUE);
    g_object_set(G_OBJECT(m_appsink), "sync", FALSE, nullptr);
    m_signalCallbackState = new std::shared_ptr<CallbackState>(m_callbackState);
    m_newSampleHandlerId = g_signal_connect_data(m_appsink, "new-sample", G_CALLBACK(onNewSample),
                                                 m_signalCallbackState,
                                                 [](gpointer data, GClosure *) {
                                                     delete static_cast<std::shared_ptr<CallbackState> *>(data);
                                                 },
                                                 G_CONNECT_DEFAULT);
    if (m_newSampleHandlerId == 0) {
        delete m_signalCallbackState;
        m_signalCallbackState = nullptr;
    }
}

std::optional<VideoFrameSample> GstAppSinkFrameSource::pullSample() {
    if (!m_appsink) return std::nullopt;

    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(m_appsink));
    if (!sample) return std::nullopt;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (!buffer || !caps) {
        gst_sample_unref(sample);
        return std::nullopt;
    }

    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *formatName = gst_structure_get_string(s, "format");
    if (!formatName || g_strcmp0(formatName, "RGBA") != 0) {
        gst_sample_unref(sample);
        return std::nullopt;
    }

    int width = 0, height = 0;
    if (!gst_structure_get_int(s, "width", &width) ||
        !gst_structure_get_int(s, "height", &height)) {
        gst_sample_unref(sample);
        return std::nullopt;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return std::nullopt;
    }

    int bytesPerLine = width * 4;
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    if (meta && meta->n_planes > 0) {
        bytesPerLine = meta->stride[0];
    }

    if (bytesPerLine <= 0 || height <= 0) {
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return std::nullopt;
    }

    const gint64 needed = static_cast<gint64>(bytesPerLine) * height;
    if (needed <= 0 || needed > static_cast<gint64>(map.size)) {
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return std::nullopt;
    }

    const int dataSize = static_cast<int>(needed);
    QByteArray bytes(reinterpret_cast<const char *>(map.data), dataSize);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    VideoFrameSample result;
    result.bytes = std::move(bytes);
    result.width = width;
    result.height = height;
    result.bytesPerLine = bytesPerLine;
    result.format = QStringLiteral("RGBA");

    return result;
}

GstFlowReturn GstAppSinkFrameSource::onNewSample(GstAppSink *appsink, gpointer userData) {
    Q_UNUSED(appsink);
    auto state = *static_cast<std::shared_ptr<CallbackState> *>(userData);
    std::function<void()> callback;
    {
        std::lock_guard lock(state->mutex);
        callback = state->frameAvailableCallback;
        if (callback) {
            state->callbacksInFlight++;
        }
    }

    if (callback) {
        activeCallbackDepthByState[state.get()]++;
        callback();
        if (--activeCallbackDepthByState[state.get()] == 0) {
            activeCallbackDepthByState.erase(state.get());
        }
        std::lock_guard lock(state->mutex);
        state->callbacksInFlight--;
        state->idle.notify_all();
    }
    return GST_FLOW_OK;
}

#include "backend/VideoFrameBridge.h"

#include <gst/video/gstvideometa.h>

VideoFrameBridge::VideoFrameBridge(GstElement *appsink, QObject *parent)
    : QObject(parent), m_appsink(appsink) {}

VideoFrameBridge::~VideoFrameBridge() {
    if (m_appsink) {
        g_signal_handlers_disconnect_by_data(m_appsink, this);
    }
}

void VideoFrameBridge::start() {
    if (!m_appsink || m_started) return;
    m_started = true;
    gst_app_sink_set_emit_signals(GST_APP_SINK(m_appsink), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(m_appsink), 2);
    gst_app_sink_set_drop(GST_APP_SINK(m_appsink), TRUE);
    g_object_set(G_OBJECT(m_appsink), "sync", FALSE, nullptr);
    g_signal_connect(m_appsink, "new-sample", G_CALLBACK(onNewSample), this);
}

GstFlowReturn VideoFrameBridge::onNewSample(GstAppSink *appsink, gpointer userData) {
    auto *self = static_cast<VideoFrameBridge *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (!buffer || !caps) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *formatName = gst_structure_get_string(s, "format");
    if (!formatName || g_strcmp0(formatName, "RGBA") != 0) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    int width = 0, height = 0;
    if (!gst_structure_get_int(s, "width", &width) ||
        !gst_structure_get_int(s, "height", &height)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    int bytesPerLine = width * 4;
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    if (meta && meta->n_planes > 0) {
        bytesPerLine = meta->stride[0];
    }

    QImage frame(map.data, width, height, bytesPerLine, QImage::Format_RGBA8888);
    QImage copy = frame.copy();

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    emit self->frameReady(copy);
    return GST_FLOW_OK;
}

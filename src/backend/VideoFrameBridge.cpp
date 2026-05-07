#include "backend/VideoFrameBridge.h"

VideoFrameBridge::VideoFrameBridge(GstElement *appsink, QObject *parent)
    : QObject(parent), appsink_(appsink) {}

VideoFrameBridge::~VideoFrameBridge() {
    if (appsink_) {
        g_signal_handlers_disconnect_by_data(appsink_, this);
    }
}

void VideoFrameBridge::start() {
    gst_app_sink_set_emit_signals(GST_APP_SINK(appsink_), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink_), 2);
    gst_app_sink_set_drop(GST_APP_SINK(appsink_), TRUE);
    g_object_set(G_OBJECT(appsink_), "sync", FALSE, nullptr);
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(onNewSample), this);
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

    QImage frame(map.data, width, height, QImage::Format_RGBA8888);
    QImage copy = frame.copy();

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    emit self->frameReady(copy);
    return GST_FLOW_OK;
}

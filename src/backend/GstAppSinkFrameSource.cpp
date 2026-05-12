#include "backend/GstAppSinkFrameSource.h"

#include <gst/video/gstvideometa.h>

GstAppSinkFrameSource::GstAppSinkFrameSource(GstElement *appsink)
    : m_appsink(appsink) {}

GstAppSinkFrameSource::~GstAppSinkFrameSource() = default;

void GstAppSinkFrameSource::start() {
    if (m_started || !m_appsink) return;
    m_started = true;
    gst_app_sink_set_emit_signals(GST_APP_SINK(m_appsink), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(m_appsink), 2);
    gst_app_sink_set_drop(GST_APP_SINK(m_appsink), TRUE);
    g_object_set(G_OBJECT(m_appsink), "sync", FALSE, nullptr);
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

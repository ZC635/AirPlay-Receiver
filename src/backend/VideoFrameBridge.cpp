#include "backend/VideoFrameBridge.h"

VideoFrameBridge::VideoFrameBridge(GstElement *appsink, QObject *parent)
    : QObject(parent), m_source(new GstAppSinkFrameSource(appsink)), m_ownsSource(true), m_appsink(appsink) {}

VideoFrameBridge::VideoFrameBridge(AppSinkFrameSource *source, QObject *parent)
    : QObject(parent), m_source(source) {}

VideoFrameBridge::~VideoFrameBridge() {
    if (m_appsink) {
        g_signal_handlers_disconnect_by_data(m_appsink, this);
    }
    if (m_ownsSource) {
        delete m_source;
    }
}

void VideoFrameBridge::start() {
    if (m_started) return;
    m_started = true;
    if (m_source) {
        m_source->start();
    }
    if (m_appsink) {
        g_signal_connect(m_appsink, "new-sample", G_CALLBACK(onNewSample), this);
    }
}

void VideoFrameBridge::processFrame() {
    auto sample = m_source->pullSample();
    if (!sample) return;
    if (sample->bytes.isEmpty() || sample->width <= 0 || sample->height <= 0) return;
    if (sample->format != QStringLiteral("RGBA")) return;
    if (sample->bytesPerLine < sample->width * 4) return;

    QImage frame(reinterpret_cast<const uchar *>(sample->bytes.constData()),
                 sample->width, sample->height,
                 sample->bytesPerLine, QImage::Format_RGBA8888);
    QImage copy = frame.copy();
    emit frameReady(copy);
}

GstFlowReturn VideoFrameBridge::onNewSample(GstAppSink *appsink, gpointer userData) {
    Q_UNUSED(appsink);
    auto *self = static_cast<VideoFrameBridge *>(userData);
    self->processFrame();
    return GST_FLOW_OK;
}

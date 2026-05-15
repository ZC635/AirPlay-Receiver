#include "backend/VideoFrameBridge.h"

VideoFrameBridge::VideoFrameBridge(GstElement *appsink, QObject *parent)
    : QObject(parent), m_source(new GstAppSinkFrameSource(appsink)), m_ownsSource(true) {}

VideoFrameBridge::VideoFrameBridge(AppSinkFrameSource *source, QObject *parent)
    : QObject(parent), m_source(source) {}

VideoFrameBridge::~VideoFrameBridge() {
    if (m_source) {
        m_source->setFrameAvailableCallback({});
    }
    if (m_ownsSource) {
        delete m_source;
    }
}

void VideoFrameBridge::start() {
    if (m_started) return;
    m_started = true;
    if (m_source) {
        m_source->setFrameAvailableCallback([this]() { processFrame(); });
        m_source->start();
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

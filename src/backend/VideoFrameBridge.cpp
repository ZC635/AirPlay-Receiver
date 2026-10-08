#include "backend/VideoFrameBridge.h"

#include <utility>

VideoFrameBridge::VideoFrameBridge(AppSinkFrameSource *source, QObject *parent)
    : QObject(parent), m_source(source) {}

VideoFrameBridge::VideoFrameBridge(std::unique_ptr<AppSinkFrameSource> source, QObject *parent)
    : QObject(parent), m_ownedSource(std::move(source)), m_source(m_ownedSource.get()) {}

VideoFrameBridge::~VideoFrameBridge() {
    if (m_source) {
        m_source->setFrameAvailableCallback({});
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
    if (!sample) {
        if (m_observation) m_observation->observe(VideoObservation::BridgeRejected, "no_sample");
        return;
    }
    if (m_observation) m_observation->observe(VideoObservation::BridgeSample);
    if (sample->bytes.isEmpty() || sample->width <= 0 || sample->height <= 0) {
        if (m_observation) m_observation->observe(VideoObservation::BridgeRejected, "empty_or_dimensions");
        return;
    }
    if (sample->format != QStringLiteral("RGBA")) {
        if (m_observation) m_observation->observe(VideoObservation::BridgeRejected, "format");
        return;
    }
    if (sample->bytesPerLine < sample->width * 4) {
        if (m_observation) m_observation->observe(VideoObservation::BridgeRejected, "stride");
        return;
    }

    QImage frame(reinterpret_cast<const uchar *>(sample->bytes.constData()),
                 sample->width, sample->height,
                 sample->bytesPerLine, QImage::Format_RGBA8888);
    QImage copy = frame.copy();
    if (m_observation) m_observation->observe(VideoObservation::BridgeAccepted);
    emit frameReady(copy);
}

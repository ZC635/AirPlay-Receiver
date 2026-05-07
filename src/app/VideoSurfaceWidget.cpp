#include "app/VideoSurfaceWidget.h"

#include "app/VideoRenderGeometry.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMetaObject>
#include <QResizeEvent>

VideoSurfaceWidget::VideoSurfaceWidget(QWidget *parent)
    : QWidget(parent) {
    setObjectName("videoSurface");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
}

VideoSurfaceWidget::~VideoSurfaceWidget() = default;

void VideoSurfaceWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (!m_cachedFrame.isNull() && isVisible()) {
        repaint();
        return;
    }
    update();
}

void VideoSurfaceWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    paintFallback(painter);
}

void VideoSurfaceWidget::onFrameReady(QImage frame) {
    QImage converted = frame.format() == QImage::Format_RGBA8888 ? frame : frame.convertToFormat(QImage::Format_RGBA8888);
    {
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame = converted;
    }
    QMetaObject::invokeMethod(this, &VideoSurfaceWidget::processPendingFrame, Qt::QueuedConnection);
}

void VideoSurfaceWidget::processPendingFrame() {
    {
        QMutexLocker locker(&m_frameMutex);
        if (m_pendingFrame.isNull()) {
            return;
        }
        m_cachedFrame = m_pendingFrame;
        m_pendingFrame = QImage();
    }
    update();
}

void VideoSurfaceWidget::reset() {
    {
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame = QImage();
    }
    m_cachedFrame = QImage();
    update();
}

void VideoSurfaceWidget::setVideoFitMode(bool fit) {
    if (m_videoFitMode == fit) {
        return;
    }

    m_videoFitMode = fit;
    update();
}

void VideoSurfaceWidget::paintFallback(QPainter &painter) {
    painter.fillRect(rect(), Qt::white);
    if (m_cachedFrame.isNull()) {
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(videoTargetRect(m_cachedFrame.size(), size(), m_videoFitMode), m_cachedFrame);
}

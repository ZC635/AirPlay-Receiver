#include "app/VideoSurfaceWidget.h"

#include "app/D3D11VideoRenderer.h"
#include "app/VideoRenderGeometry.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

#include <windows.h>

VideoSurfaceWidget::VideoSurfaceWidget(QWidget *parent)
    : QWidget(parent) {
    setObjectName("videoSurface");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
}

VideoSurfaceWidget::~VideoSurfaceWidget() = default;

void VideoSurfaceWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);

    if (ensureRenderer()) {
        m_renderer->resize(e->size().width(), e->size().height());
    }
    renderCurrentFrame();
}

void VideoSurfaceWidget::paintEvent(QPaintEvent *) {
    if (!m_cachedFrame.isNull() && ensureRenderer()) {
        renderCurrentFrame();
        return;
    }

    QPainter painter(this);
    paintFallback(painter);
}

void VideoSurfaceWidget::onFrameReady(QImage frame) {
    m_cachedFrame = frame.convertToFormat(QImage::Format_RGBA8888);
    renderCurrentFrame();
}

void VideoSurfaceWidget::reset() {
    m_cachedFrame = QImage();
    m_rendererUnavailable = false;
    if (m_renderer) {
        m_renderer->resetFrame();
        m_renderer.reset();
    }
    update();
}

void VideoSurfaceWidget::setVideoFitMode(bool fit) {
    if (m_videoFitMode == fit) {
        return;
    }

    m_videoFitMode = fit;
    renderCurrentFrame();
}

bool VideoSurfaceWidget::ensureRenderer() {
    if (m_renderer && m_renderer->isInitialized()) {
        return true;
    }
    if (!isVisible() || width() <= 0 || height() <= 0) {
        return false;
    }
    if (m_rendererUnavailable) {
        return false;
    }

    WId id = winId();
    if (!id) {
        return false;
    }

    auto renderer = std::make_unique<D3D11VideoRenderer>();
    if (!renderer->initialize(reinterpret_cast<HWND>(id))) {
        m_rendererUnavailable = true;
        return false;
    }

    renderer->resize(width(), height());
    m_renderer = std::move(renderer);
    return true;
}

void VideoSurfaceWidget::renderCurrentFrame() {
    if (m_cachedFrame.isNull()) {
        if (m_renderer && m_renderer->isInitialized()) {
            m_renderer->render(m_videoFitMode);
        } else {
            update();
        }
        return;
    }

    if (!ensureRenderer()) {
        update();
        return;
    }

    m_renderer->uploadFrame(m_cachedFrame);
    m_renderer->render(m_videoFitMode);
}

void VideoSurfaceWidget::paintFallback(QPainter &painter) {
    painter.fillRect(rect(), Qt::white);
    if (m_cachedFrame.isNull()) {
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(videoTargetRect(m_cachedFrame.size(), size(), m_videoFitMode), m_cachedFrame);
}

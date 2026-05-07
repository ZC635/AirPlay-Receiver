#include "app/VideoSurfaceWidget.h"

#include "app/D3D11VideoRenderer.h"
#include "app/VideoRenderGeometry.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>

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
    if (!m_d3dDisabled && !m_cachedFrame.isNull() && ensureRenderer()) {
        renderCurrentFrame();
        return;
    }

    QPainter painter(this);
    paintFallback(painter);
}

void VideoSurfaceWidget::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    m_d3dDisabled = false;
    m_consecutiveFailures = 0;
}

void VideoSurfaceWidget::onFrameReady(QImage frame) {
    QImage converted = frame.convertToFormat(QImage::Format_RGBA8888);
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
    m_textureDirty = true;
    renderCurrentFrame();
}

void VideoSurfaceWidget::reset() {
    {
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame = QImage();
    }
    m_cachedFrame = QImage();
    m_textureDirty = false;
    m_consecutiveFailures = 0;
    m_d3dDisabled = false;
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
    if (m_d3dDisabled) {
        return false;
    }
    if (m_renderer && m_renderer->isInitialized()) {
        return true;
    }
    if (!isVisible() || width() <= 0 || height() <= 0) {
        return false;
    }

    WId id = winId();
    if (!id) {
        return false;
    }

    auto renderer = std::make_unique<D3D11VideoRenderer>();
    if (!renderer->initialize(reinterpret_cast<HWND>(id))) {
        m_consecutiveFailures++;
        if (m_consecutiveFailures >= kMaxConsecutiveFailures) {
            m_d3dDisabled = true;
        }
        return false;
    }

    renderer->resize(width(), height());
    m_renderer = std::move(renderer);
    m_textureDirty = true;
    m_consecutiveFailures = 0;
    return true;
}

void VideoSurfaceWidget::renderCurrentFrame() {
    if (m_d3dDisabled) {
        update();
        return;
    }

    if (m_cachedFrame.isNull()) {
        if (m_renderer && m_renderer->isInitialized()) {
            if (!m_renderer->render(m_videoFitMode)) {
                handleRenderFailure();
            }
        } else {
            update();
        }
        return;
    }

    if (!ensureRenderer()) {
        update();
        return;
    }

    if (m_textureDirty) {
        if (!m_renderer->uploadFrame(m_cachedFrame)) {
            handleRenderFailure();
            return;
        }
        m_textureDirty = false;
    }

    if (!m_renderer->render(m_videoFitMode)) {
        handleRenderFailure();
        return;
    }

    m_consecutiveFailures = 0;
}

void VideoSurfaceWidget::handleRenderFailure() {
    m_consecutiveFailures++;
    m_renderer.reset();
    m_textureDirty = true;
    if (m_consecutiveFailures >= kMaxConsecutiveFailures) {
        m_d3dDisabled = true;
    }
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

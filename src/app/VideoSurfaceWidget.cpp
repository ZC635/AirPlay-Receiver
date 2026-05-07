#include "app/VideoSurfaceWidget.h"

#include <QPainter>

namespace {
const QColor kBackgroundColor = Qt::white;

QRectF targetRectFor(const QSizeF &sourceSize, const QSizeF &boundsSize, bool fit) {
    if (!fit || sourceSize.isEmpty() || boundsSize.isEmpty()) {
        return QRectF(QPointF(0.0, 0.0), boundsSize);
    }

    QSizeF drawSize = boundsSize;
    const qreal sourceAspect = sourceSize.width() / sourceSize.height();
    const qreal boundsAspect = boundsSize.width() / boundsSize.height();
    if (sourceAspect > boundsAspect) {
        drawSize.setHeight(boundsSize.width() / sourceAspect);
    } else {
        drawSize.setWidth(boundsSize.height() * sourceAspect);
    }

    return QRectF(
        (boundsSize.width() - drawSize.width()) * 0.5,
        (boundsSize.height() - drawSize.height()) * 0.5,
        drawSize.width(),
        drawSize.height());
}
}

VideoSurfaceWidget::VideoSurfaceWidget(QWidget *parent)
    : QOpenGLWidget(parent) {
    setObjectName("videoSurface");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VideoSurfaceWidget::~VideoSurfaceWidget() {
    makeCurrent();
    m_texture.reset();
    doneCurrent();
}

void VideoSurfaceWidget::initializeGL() {
    initializeOpenGLFunctions();
}

void VideoSurfaceWidget::resizeGL(int, int) {
    renderTexture();
}

void VideoSurfaceWidget::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.fillRect(rect(), kBackgroundColor);
    if (!m_paintCache.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(targetRectFor(m_paintCache.size(), size(), m_videoFitMode), m_paintCache);
    }
    p.end();
    QOpenGLWidget::paintEvent(e);
}

void VideoSurfaceWidget::paintGL() {
    {
        QMutexLocker locker(&m_frameMutex);
        if (!m_pendingFrame.isNull()) {
            updateTexture(m_pendingFrame);
            m_pendingFrame = QImage();
        }
    }
    renderTexture();
}

void VideoSurfaceWidget::renderTexture() {
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_texture || !m_texture->isCreated()) return;

    m_texture->bind();

    float texW = static_cast<float>(m_texture->width());
    float texH = static_cast<float>(m_texture->height());
    float widgetW = static_cast<float>(width());
    float widgetH = static_cast<float>(height());
    const QRectF target = targetRectFor(QSizeF(texW, texH), QSizeF(widgetW, widgetH), m_videoFitMode);
    const float x = static_cast<float>(target.x());
    const float y = static_cast<float>(target.y());
    const float drawW = static_cast<float>(target.width());
    const float drawH = static_cast<float>(target.height());

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, widgetW, widgetH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + drawW, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + drawW, y + drawH);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + drawH);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    m_texture->release();
}

void VideoSurfaceWidget::updateTexture(const QImage &frame) {
    QImage gl = frame.convertToFormat(QImage::Format_RGBA8888);
    m_paintCache = gl;
    bool recreate = !m_texture ||
                    m_texture->width() != gl.width() ||
                    m_texture->height() != gl.height();
    if (recreate) {
        m_texture = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
        m_texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
        m_texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
        m_texture->setSize(gl.width(), gl.height());
        m_texture->allocateStorage();
    }
    m_texture->setData(0, QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                       gl.constBits());
}

void VideoSurfaceWidget::onFrameReady(QImage frame) {
    {
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame = std::move(frame);
    }
    update();
}

void VideoSurfaceWidget::reset() {
    {
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame = QImage();
    }
    update();
}

void VideoSurfaceWidget::setVideoFitMode(bool fit) {
    m_videoFitMode = fit;
    update();
}

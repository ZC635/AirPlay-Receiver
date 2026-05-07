#include "app/VideoSurfaceWidget.h"

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

void VideoSurfaceWidget::paintGL() {
    {
        QMutexLocker locker(&m_frameMutex);
        if (!m_pendingFrame.isNull()) {
            updateTexture(m_pendingFrame);
            m_pendingFrame = QImage();
        }
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_texture || !m_texture->isCreated()) return;

    m_texture->bind();

    float texW = static_cast<float>(m_texture->width());
    float texH = static_cast<float>(m_texture->height());
    float widgetW = static_cast<float>(width());
    float widgetH = static_cast<float>(height());
    float drawW = widgetW;
    float drawH = widgetH;

    if (texW > 0.0f && texH > 0.0f) {
        float videoAspect = texW / texH;
        float widgetAspect = widgetW / widgetH;

        if (m_videoFitMode) {
            if (videoAspect > widgetAspect)
                drawH = widgetW / videoAspect;
            else
                drawW = widgetH * videoAspect;
        } else {
            if (videoAspect > widgetAspect)
                drawW = widgetH * videoAspect;
            else
                drawH = widgetW / videoAspect;
        }
    }

    float x = (widgetW - drawW) * 0.5f;
    float y = (widgetH - drawH) * 0.5f;

    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + drawW, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + drawW, y + drawH);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y + drawH);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    m_texture->release();
}

void VideoSurfaceWidget::updateTexture(const QImage &frame) {
    QImage gl = frame.convertToFormat(QImage::Format_RGBA8888);
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

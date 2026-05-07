#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLTexture>
#include <QImage>
#include <QMutex>
#include <memory>

class VideoSurfaceWidget final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit VideoSurfaceWidget(QWidget *parent = nullptr);
    ~VideoSurfaceWidget() override;
    void reset();
    void setVideoFitMode(bool fit);

public slots:
    void onFrameReady(QImage frame);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void updateTexture(const QImage &frame);

    std::unique_ptr<QOpenGLTexture> m_texture;
    QImage m_pendingFrame;
    QMutex m_frameMutex;
    bool m_videoFitMode = false;
};

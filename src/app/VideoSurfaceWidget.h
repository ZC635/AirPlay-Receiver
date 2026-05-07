#pragma once

#include <QImage>
#include <QMutex>
#include <QWidget>
#include <memory>

class D3D11VideoRenderer;
class QPainter;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;

class VideoSurfaceWidget final : public QWidget {
    Q_OBJECT

public:
    explicit VideoSurfaceWidget(QWidget *parent = nullptr);
    ~VideoSurfaceWidget() override;
    void reset();
    void setVideoFitMode(bool fit);

public slots:
    void onFrameReady(QImage frame);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    bool ensureRenderer();
    bool renderCurrentFrame();
    void paintFallback(QPainter &painter);
    void processPendingFrame();
    void handleRenderFailure();

    std::unique_ptr<D3D11VideoRenderer> m_renderer;
    QMutex m_frameMutex;
    QImage m_pendingFrame;
    QImage m_cachedFrame;
    bool m_textureDirty = false;
    bool m_videoFitMode = false;
    int m_consecutiveFailures = 0;
    bool m_d3dDisabled = false;
    static constexpr int kMaxConsecutiveFailures = 3;
};

#pragma once

#include <QImage>
#include <QWidget>
#include <memory>

class D3D11VideoRenderer;
class QPainter;
class QPaintEvent;
class QResizeEvent;

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

private:
    bool ensureRenderer();
    void renderCurrentFrame();
    void paintFallback(QPainter &painter);

    std::unique_ptr<D3D11VideoRenderer> m_renderer;
    QImage m_cachedFrame;
    bool m_videoFitMode = false;
    bool m_rendererUnavailable = false;
};

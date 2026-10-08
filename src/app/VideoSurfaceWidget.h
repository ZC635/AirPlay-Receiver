#pragma once

#include <QImage>
#include <QMutex>
#include <QWidget>
#include <memory>
#include "diagnostics/VideoObservation.h"

class QPainter;
class QPaintEvent;
class QResizeEvent;

class VideoSurfaceWidget final : public QWidget {
    Q_OBJECT

public:
    explicit VideoSurfaceWidget(QWidget *parent = nullptr);
    ~VideoSurfaceWidget() override;
    void setVideoObservation(std::shared_ptr<VideoObservation> observation) { m_observation = std::move(observation); }
    void reset();
    void setVideoFitMode(bool fit);

public slots:
    void onFrameReady(QImage frame);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    void paintFallback(QPainter &painter);
    void processPendingFrame();

    QMutex m_frameMutex;
    QImage m_pendingFrame;
    QImage m_cachedFrame;
    bool m_videoFitMode = false;
    std::shared_ptr<VideoObservation> m_observation;
};

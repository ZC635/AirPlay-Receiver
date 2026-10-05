#pragma once

#include <QObject>
#include <QImage>

#include <memory>

#include "backend/AppSinkFrameSource.h"

class VideoFrameBridge : public QObject {
    Q_OBJECT

public:
    // Non-owning: caller must ensure source outlives this bridge.
    explicit VideoFrameBridge(AppSinkFrameSource *source, QObject *parent = nullptr);
    explicit VideoFrameBridge(std::unique_ptr<AppSinkFrameSource> source, QObject *parent = nullptr);
    ~VideoFrameBridge() override;
    void start();
    void processFrame();

signals:
    void frameReady(QImage frame);

private:
    std::unique_ptr<AppSinkFrameSource> m_ownedSource;
    AppSinkFrameSource *m_source = nullptr;
    bool m_started = false;
};

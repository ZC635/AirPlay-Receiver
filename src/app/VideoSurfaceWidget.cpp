#include "app/VideoSurfaceWidget.h"

#include <QPainter>
#include <QPalette>
#include <QPaintEvent>

VideoSurfaceWidget::VideoSurfaceWidget(QWidget *parent)
    : QWidget(parent) {
    setObjectName("videoSurface");
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);

    defaultWindowColor_ = palette().color(QPalette::Window);

    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, Qt::black);
    setPalette(palette);
    setAutoFillBackground(false);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VideoSurfaceWidget::paintEvent(QPaintEvent *event) {
    if (!requestClear_) {
        return;
    }
    requestClear_ = false;
    QPainter painter(this);
    painter.fillRect(event->rect(), defaultWindowColor_);
}

void VideoSurfaceWidget::reset() {
    requestClear_ = true;
    update();
}

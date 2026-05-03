#include "app/VideoSurfaceWidget.h"

#include <QPalette>

VideoSurfaceWidget::VideoSurfaceWidget(QWidget *parent)
    : QWidget(parent) {
    setObjectName("videoSurface");
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);

    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, Qt::black);
    setPalette(palette);
    setAutoFillBackground(false);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

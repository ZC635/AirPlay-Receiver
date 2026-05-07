#include "app/VideoRenderGeometry.h"

QRectF videoTargetRect(QSizeF sourceSize, QSizeF boundsSize, bool fit) {
    if (sourceSize.width() <= 0.0 || sourceSize.height() <= 0.0 ||
        boundsSize.width() <= 0.0 || boundsSize.height() <= 0.0) {
        return QRectF();
    }

    if (!fit) {
        return QRectF(QPointF(0, 0), boundsSize);
    }

    const double sourceAspect = sourceSize.width() / sourceSize.height();
    const double boundsAspect = boundsSize.width() / boundsSize.height();

    QSizeF targetSize = boundsSize;
    if (sourceAspect > boundsAspect) {
        targetSize.setHeight(boundsSize.width() / sourceAspect);
    } else {
        targetSize.setWidth(boundsSize.height() * sourceAspect);
    }

    return QRectF(QPointF((boundsSize.width() - targetSize.width()) * 0.5,
                         (boundsSize.height() - targetSize.height()) * 0.5),
                  targetSize);
}

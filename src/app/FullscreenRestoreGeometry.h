#pragma once

#include <QList>
#include <QRect>
#include <algorithm>

inline QRect fullscreenRestoreGeometry(const QRect &original, const QRect &qtRestored,
                                      const QList<QRect> &availableScreens) {
    for (const QRect &available : availableScreens) {
        if (available.intersects(original)) {
            return original;
        }
    }
    for (const QRect &available : availableScreens) {
        if (available.intersects(qtRestored)) {
            return qtRestored;
        }
    }
    for (const QRect &available : availableScreens) {
        if (available.isEmpty()) {
            continue;
        }
        const int width = std::min(original.width(), available.width());
        const int height = std::min(original.height(), available.height());
        return QRect(std::clamp(original.x(), available.left(), available.right() - width + 1),
                     std::clamp(original.y(), available.top(), available.bottom() - height + 1),
                     width, height);
    }
    return qtRestored;
}

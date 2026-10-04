#pragma once

#include <QList>
#include <QRect>
#include <algorithm>

inline QRect fullscreenRestoreGeometry(const QRect &original, const QRect &qtRestored,
                                      const QList<QRect> &availableScreens,
                                      const char **strategy = nullptr) {
    for (const QRect &available : availableScreens) {
        if (available.intersects(original)) {
            if (strategy) *strategy = "original";
            return original;
        }
    }
    for (const QRect &available : availableScreens) {
        if (available.intersects(qtRestored)) {
            if (strategy) *strategy = "qt_restored";
            return qtRestored;
        }
    }
    for (const QRect &available : availableScreens) {
        if (available.isEmpty()) {
            continue;
        }
        const int width = std::min(original.width(), available.width());
        const int height = std::min(original.height(), available.height());
        if (strategy) *strategy = "clamped";
        return QRect(std::clamp(original.x(), available.left(), available.right() - width + 1),
                     std::clamp(original.y(), available.top(), available.bottom() - height + 1),
                     width, height);
    }
    if (strategy) *strategy = "no_screen";
    return qtRestored;
}

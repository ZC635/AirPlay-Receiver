#pragma once

#include "platform/AspectRatioSizing.h"

#include <QtGlobal>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct WindowsNativeEventResult {
    bool handled = false;
};

bool setNativeAlwaysOnTop(WId windowId, bool enabled);
bool setWindowBorderColor(WId windowId, bool enabled);
void makeNativeOverlay(QWidget *widget);
void raiseNativeOverlay(QWidget *widget);
AspectRatioFrameMargins frameMarginsFor(const QWidget &widget);
AspectRatioSizeConstraints sizeConstraintsFor(const QWidget &widget, const AspectRatioFrameMargins &margins);

WindowsNativeEventResult handleNativeWindowBehaviorEvent(
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    qintptr *result,
    const QWidget *widget = nullptr,
    bool aspectRatioLock = false,
    int videoWidth = 0,
    int videoHeight = 0);

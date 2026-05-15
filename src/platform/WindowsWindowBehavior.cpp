#include "platform/WindowsWindowBehavior.h"

#include <algorithm>
#include <dwmapi.h>

bool setNativeAlwaysOnTop(WId windowId, bool enabled) {
    HWND window = reinterpret_cast<HWND>(windowId);
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }

    return SetWindowPos(
               window,
               enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
               0,
               0,
               0,
               0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
}

bool setWindowBorderColor(WId windowId, bool enabled) {
    HWND window = reinterpret_cast<HWND>(windowId);
    if (window == nullptr || !IsWindow(window)) {
        return false;
    }
    const COLORREF blue = RGB(0x33, 0x96, 0xF3);
    const COLORREF none = 0xFFFFFFFE;
    return SUCCEEDED(DwmSetWindowAttribute(
        window, 34, enabled ? &blue : &none, sizeof(COLORREF)));
}

void makeNativeOverlay(QWidget *widget) {
    widget->setAttribute(Qt::WA_NativeWindow, true);
    static_cast<void>(widget->winId());
}

void raiseNativeOverlay(QWidget *widget) {
    widget->raise();
    HWND window = reinterpret_cast<HWND>(widget->winId());
    if (window == nullptr || !IsWindow(window)) {
        return;
    }

    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

AspectRatioFrameMargins frameMarginsFor(const QWidget &widget) {
    const QRect frame = widget.frameGeometry();
    const QRect client = widget.geometry();
    const int left = client.left() - frame.left();
    const int top = client.top() - frame.top();
    return AspectRatioFrameMargins{
        left,
        top,
        frame.width() - client.width() - left,
        frame.height() - client.height() - top};
}

AspectRatioSizeConstraints sizeConstraintsFor(const QWidget &widget, const AspectRatioFrameMargins &margins) {
    const int horizontalFrame = std::max(0, margins.left) + std::max(0, margins.right);
    const int verticalFrame = std::max(0, margins.top) + std::max(0, margins.bottom);
    return AspectRatioSizeConstraints{
        widget.minimumWidth() + horizontalFrame,
        widget.minimumHeight() + verticalFrame,
        widget.maximumWidth() + horizontalFrame,
        widget.maximumHeight() + verticalFrame};
}

WindowsNativeEventResult handleNativeWindowBehaviorEvent(
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    qintptr *result,
    const QWidget *widget,
    bool aspectRatioLock,
    int videoWidth,
    int videoHeight) {
    if (message == WM_WINDOWPOSCHANGING) {
        auto *wp = reinterpret_cast<WINDOWPOS *>(lParam);
        if (wp != nullptr) {
            updateWindowPosCopyBitsForResize(*wp);
        }
        return {};
    }

    if (message != WM_SIZING || widget == nullptr || !aspectRatioLock || videoWidth <= 0 || videoHeight <= 0) {
        return {};
    }

    auto *rect = reinterpret_cast<RECT *>(lParam);
    const double targetRatio = static_cast<double>(videoWidth) / videoHeight;
    const AspectRatioFrameMargins margins = frameMarginsFor(*widget);
    if (rect == nullptr || !adjustWindowRectForAspectRatio(
            *rect,
            static_cast<unsigned int>(wParam),
            targetRatio,
            margins,
            sizeConstraintsFor(*widget, margins))) {
        return {};
    }

    if (result != nullptr) {
        *result = TRUE;
    }
    return WindowsNativeEventResult{true};
}

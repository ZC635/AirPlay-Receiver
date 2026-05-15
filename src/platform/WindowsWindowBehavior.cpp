#include "platform/WindowsWindowBehavior.h"

#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <QPoint>

namespace {
int rectWidth(const RECT &rect) {
    return static_cast<int>(rect.right - rect.left);
}

int rectHeight(const RECT &rect) {
    return static_cast<int>(rect.bottom - rect.top);
}

int scaledLogicalValue(int logicalValue, double scale) {
    return static_cast<int>(std::lround(logicalValue * scale));
}

QRect targetGeometryFor(const QWidget &window, const QWidget *target) {
    if (target == nullptr || target == &window || !window.isAncestorOf(target) || target->width() <= 0 || target->height() <= 0) {
        return QRect(QPoint(0, 0), window.size());
    }
    return QRect(target->mapTo(&window, QPoint(0, 0)), target->size());
}

bool nativeWindowGeometryFor(const QWidget &widget, RECT &outerRect, RECT &clientRect) {
    const HWND hwnd = reinterpret_cast<HWND>(widget.effectiveWinId());
    if (hwnd == nullptr || !IsWindow(hwnd) || !GetWindowRect(hwnd, &outerRect)) {
        return false;
    }

    RECT localClient{};
    if (!GetClientRect(hwnd, &localClient)) {
        return false;
    }

    POINT clientPoints[2] = {{localClient.left, localClient.top}, {localClient.right, localClient.bottom}};
    if (MapWindowPoints(hwnd, nullptr, clientPoints, 2) == 0 && GetLastError() != 0) {
        return false;
    }

    clientRect = RECT{clientPoints[0].x, clientPoints[0].y, clientPoints[1].x, clientPoints[1].y};
    return rectWidth(clientRect) > 0 && rectHeight(clientRect) > 0;
}

AspectRatioSizeConstraints nativeSizeConstraintsFor(
    const QWidget &widget,
    const RECT &clientRect,
    const AspectRatioFrameMargins &margins) {
    const int horizontalFrame = std::max(0, margins.left) + std::max(0, margins.right);
    const int verticalFrame = std::max(0, margins.top) + std::max(0, margins.bottom);
    const double scaleX = widget.width() > 0 ? static_cast<double>(rectWidth(clientRect)) / widget.width() : 1.0;
    const double scaleY = widget.height() > 0 ? static_cast<double>(rectHeight(clientRect)) / widget.height() : 1.0;
    return AspectRatioSizeConstraints{
        scaledLogicalValue(widget.minimumWidth(), scaleX) + horizontalFrame,
        scaledLogicalValue(widget.minimumHeight(), scaleY) + verticalFrame,
        scaledLogicalValue(widget.maximumWidth(), scaleX) + horizontalFrame,
        scaledLogicalValue(widget.maximumHeight(), scaleY) + verticalFrame};
}
}

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

AspectRatioFrameMargins aspectTargetMarginsFor(const QWidget &window, const QWidget &target) {
    const AspectRatioFrameMargins frameMargins = frameMarginsFor(window);
    if (&window == &target || !window.isAncestorOf(&target) || target.width() <= 0 || target.height() <= 0) {
        return frameMargins;
    }

    const QPoint targetTopLeft = target.mapTo(&window, QPoint(0, 0));
    const int left = frameMargins.left + targetTopLeft.x();
    const int top = frameMargins.top + targetTopLeft.y();
    return AspectRatioFrameMargins{
        left,
        top,
        window.frameGeometry().width() - target.width() - left,
        window.frameGeometry().height() - target.height() - top};
}

AspectRatioFrameMargins aspectTargetMarginsFromNativeGeometry(
    const RECT &outerRect,
    const RECT &clientRect,
    QSize logicalClientSize,
    QRect logicalTargetGeometry) {
    if (rectWidth(outerRect) <= 0 || rectHeight(outerRect) <= 0 ||
        rectWidth(clientRect) <= 0 || rectHeight(clientRect) <= 0 ||
        logicalClientSize.width() <= 0 || logicalClientSize.height() <= 0 ||
        logicalTargetGeometry.width() <= 0 || logicalTargetGeometry.height() <= 0) {
        return {};
    }

    const double scaleX = static_cast<double>(rectWidth(clientRect)) / logicalClientSize.width();
    const double scaleY = static_cast<double>(rectHeight(clientRect)) / logicalClientSize.height();
    const int targetLeft = clientRect.left + scaledLogicalValue(logicalTargetGeometry.x(), scaleX);
    const int targetTop = clientRect.top + scaledLogicalValue(logicalTargetGeometry.y(), scaleY);
    const int targetRight = targetLeft + scaledLogicalValue(logicalTargetGeometry.width(), scaleX);
    const int targetBottom = targetTop + scaledLogicalValue(logicalTargetGeometry.height(), scaleY);

    return AspectRatioFrameMargins{
        targetLeft - outerRect.left,
        targetTop - outerRect.top,
        outerRect.right - targetRight,
        outerRect.bottom - targetBottom};
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
    int videoHeight,
    const QWidget *aspectTarget) {
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
    const AspectRatioFrameMargins frameMargins = frameMarginsFor(*widget);
    AspectRatioFrameMargins aspectMargins = aspectTarget != nullptr
        ? aspectTargetMarginsFor(*widget, *aspectTarget)
        : frameMargins;
    AspectRatioSizeConstraints constraints = sizeConstraintsFor(*widget, frameMargins);
    RECT nativeOuter{};
    RECT nativeClient{};
    if (nativeWindowGeometryFor(*widget, nativeOuter, nativeClient)) {
        const AspectRatioFrameMargins nativeFrameMargins = aspectTargetMarginsFromNativeGeometry(
            nativeOuter, nativeClient, widget->size(), QRect(QPoint(0, 0), widget->size()));
        aspectMargins = aspectTargetMarginsFromNativeGeometry(
            nativeOuter, nativeClient, widget->size(), targetGeometryFor(*widget, aspectTarget));
        constraints = nativeSizeConstraintsFor(*widget, nativeClient, nativeFrameMargins);
    }
    if (rect == nullptr || !adjustWindowRectForAspectRatio(
            *rect,
            static_cast<unsigned int>(wParam),
            targetRatio,
            aspectMargins,
            constraints)) {
        return {};
    }

    if (result != nullptr) {
        *result = TRUE;
    }
    return WindowsNativeEventResult{true};
}

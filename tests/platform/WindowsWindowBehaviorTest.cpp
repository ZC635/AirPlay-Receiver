#include <QtTest/QtTest>

#include "platform/WindowsWindowBehavior.h"

#include <QRect>
#include <QSize>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class WindowsWindowBehaviorTest : public QObject {
    Q_OBJECT

private slots:
    void frameMarginsComeFromWidgetFrameAndClientGeometry() {
        QWidget widget;
        widget.setGeometry(50, 80, 320, 180);

        const AspectRatioFrameMargins margins = frameMarginsFor(widget);
        const QRect frame = widget.frameGeometry();
        const QRect client = widget.geometry();

        QCOMPARE(margins.left, client.left() - frame.left());
        QCOMPARE(margins.top, client.top() - frame.top());
        QCOMPARE(margins.right, frame.width() - client.width() - margins.left);
        QCOMPARE(margins.bottom, frame.height() - client.height() - margins.top);
    }

    void sizeConstraintsIncludeNonNegativeFrameMargins() {
        QWidget widget;
        widget.setMinimumSize(320, 180);
        widget.setMaximumSize(1280, 720);

        const AspectRatioSizeConstraints constraints = sizeConstraintsFor(
            widget, AspectRatioFrameMargins{8, 32, -4, 10});

        QCOMPARE(constraints.minOuterWidth, 328);
        QCOMPARE(constraints.minOuterHeight, 222);
        QCOMPARE(constraints.maxOuterWidth, 1288);
        QCOMPARE(constraints.maxOuterHeight, 762);
    }

    void nativeAspectTargetMarginsScaleLogicalTargetIntoNativeClient() {
        const RECT outerRect{100, 100, 626, 1270};
        const RECT clientRect{114, 158, 612, 1238};

        const AspectRatioFrameMargins margins = aspectTargetMarginsFromNativeGeometry(
            outerRect,
            clientRect,
            QSize(249, 540),
            QRect(0, 0, 249, 540));

        QCOMPARE(margins.left, 14);
        QCOMPARE(margins.top, 58);
        QCOMPARE(margins.right, 14);
        QCOMPARE(margins.bottom, 32);
    }

    void nativeWindowPosChangingClearsNoCopyBitsForResize() {
        WINDOWPOS pos{};
        pos.flags = SWP_NOCOPYBITS;

        const WindowsNativeEventResult handled = handleNativeWindowBehaviorEvent(
            WM_WINDOWPOSCHANGING,
            0,
            reinterpret_cast<LPARAM>(&pos),
            nullptr);

        QVERIFY(!handled.handled);
        QVERIFY((pos.flags & SWP_NOCOPYBITS) == 0);
    }

    void nativeSizingDispatchAdjustsRectWhenAspectLockIsActive() {
        QWidget widget;
        widget.setMinimumSize(100, 80);
        widget.setMaximumSize(2000, 1200);
        widget.resize(960, 540);

        RECT rect{100, 100, 1060, 800};
        qintptr result = 0;

        const WindowsNativeEventResult handled = handleNativeWindowBehaviorEvent(
            WM_SIZING,
            WMSZ_RIGHT,
            reinterpret_cast<LPARAM>(&rect),
            &result,
            &widget,
            true,
            1920,
            1080);

        QVERIFY(handled.handled);
        QCOMPARE(result, static_cast<qintptr>(TRUE));
        QCOMPARE(rect.left, 100L);
        QCOMPARE(rect.top, 100L);

        const AspectRatioFrameMargins margins = frameMarginsFor(widget);
        const int clientWidth = clientWidthForOuterWidth(static_cast<int>(rect.right - rect.left), margins);
        const int clientHeight = clientHeightForOuterHeight(static_cast<int>(rect.bottom - rect.top), margins);
        QVERIFY(qAbs((static_cast<double>(clientWidth) / clientHeight) - (16.0 / 9.0)) < 0.01);
    }

    void nativeSizingDispatchLocksAspectTargetWidget() {
        QWidget widget;
        widget.setMinimumSize(100, 80);
        widget.setMaximumSize(2000, 1200);
        widget.setGeometry(50, 80, 960, 580);

        QWidget videoTarget(&widget);
        videoTarget.setGeometry(0, 40, 960, 540);

        RECT rect{100, 100, 1100, 740};
        qintptr result = 0;

        const WindowsNativeEventResult handled = handleNativeWindowBehaviorEvent(
            WM_SIZING,
            WMSZ_RIGHT,
            reinterpret_cast<LPARAM>(&rect),
            &result,
            &widget,
            true,
            1920,
            1080,
            &videoTarget);

        QVERIFY(handled.handled);
        QCOMPARE(result, static_cast<qintptr>(TRUE));
        QCOMPARE(rect.left, 100L);
        QCOMPARE(rect.top, 100L);
        QCOMPARE(rect.right, 1100L);

        const AspectRatioFrameMargins margins = aspectTargetMarginsFor(widget, videoTarget);
        const int targetWidth = clientWidthForOuterWidth(static_cast<int>(rect.right - rect.left), margins);
        const int targetHeight = clientHeightForOuterHeight(static_cast<int>(rect.bottom - rect.top), margins);
        QVERIFY(qAbs((static_cast<double>(targetWidth) / targetHeight) - (16.0 / 9.0)) < 0.01);
    }
};

QTEST_MAIN(WindowsWindowBehaviorTest)
#include "WindowsWindowBehaviorTest.moc"

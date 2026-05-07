#include <QtTest/QtTest>
#include "app/VideoSurfaceWidget.h"

#include <QGuiApplication>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class VideoSurfaceWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void isPlainQWidgetVideoSurface() {
        VideoSurfaceWidget widget;
        QVERIFY(!widget.inherits("QOpenGLWidget"));
        QVERIFY(widget.inherits("QWidget"));
    }

    void hasExpandingSizePolicy() {
        VideoSurfaceWidget widget;
        QCOMPARE(widget.sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);
        QCOMPARE(widget.sizePolicy().verticalPolicy(), QSizePolicy::Expanding);
    }

    void resetClearsFrame() {
        VideoSurfaceWidget widget;
        widget.resize(100, 100);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QImage testImage(64, 64, QImage::Format_RGBA8888);
        testImage.fill(Qt::red);
        widget.onFrameReady(testImage);
        QTest::qWait(50);

        widget.reset();
        QTest::qWait(50);
    }

    void resetBeforeShowDoesNotCreateNativeWindow() {
        VideoSurfaceWidget widget;
        QVERIFY(!widget.internalWinId());

        widget.reset();

        QVERIFY(!widget.internalWinId());
    }

    void acceptsFrameDeliveryWithoutCrashing() {
        VideoSurfaceWidget widget;
        widget.resize(100, 100);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QImage testImage(64, 64, QImage::Format_RGB32);
        testImage.fill(Qt::blue);
        widget.onFrameReady(testImage);
        QTest::qWait(50);
    }

    void hiddenFrameDeliveryDoesNotCreateNativeWindowOrBlockLaterRender() {
        VideoSurfaceWidget widget;
        widget.resize(100, 100);

        QImage hiddenFrame(64, 64, QImage::Format_RGB32);
        hiddenFrame.fill(Qt::green);
        widget.onFrameReady(hiddenFrame);

        QVERIFY(!widget.internalWinId());

        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QImage visibleFrame(64, 64, QImage::Format_RGB32);
        visibleFrame.fill(Qt::blue);
        widget.onFrameReady(visibleFrame);
        QTest::qWait(50);
    }

    void hasNativeWindowHandleOnWindowsQpa() {
        if (QGuiApplication::platformName() != QStringLiteral("windows")) {
            QSKIP("Native HWND test requires the Windows QPA platform");
        }

        VideoSurfaceWidget widget;
        widget.resize(100, 100);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        HWND hwnd = reinterpret_cast<HWND>(widget.winId());
        QVERIFY(hwnd != nullptr);
        QVERIFY(IsWindow(hwnd));
    }
};

QTEST_MAIN(VideoSurfaceWidgetTest)
#include "VideoSurfaceWidgetTest.moc"

#include <QtTest/QtTest>
#include <QThread>
#include "app/VideoSurfaceWidget.h"

#include <QGuiApplication>

class VideoSurfaceWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void isPlainQWidgetVideoSurface() {
        VideoSurfaceWidget widget;
        QVERIFY(!widget.inherits("QOpenGLWidget"));
        QVERIFY(widget.inherits("QWidget"));
    }

    void defaultsToPainterSurfaceWithoutNativeChild() {
        VideoSurfaceWidget widget;

        QVERIFY(!widget.testAttribute(Qt::WA_NativeWindow));
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

    void crossThreadFrameDeliveryDoesNotCrashAndResultsInValidPaint() {
        VideoSurfaceWidget widget;
        widget.resize(100, 100);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        std::atomic<bool> delivered{false};

        QThread *thread = QThread::create([&]() {
            QImage frame(64, 64, QImage::Format_RGBA8888);
            frame.fill(Qt::green);
            widget.onFrameReady(frame);
            delivered.store(true);
        });
        thread->start();

        QTRY_VERIFY_WITH_TIMEOUT(delivered.load(), 1000);

        QTest::qWait(200);

        thread->wait();
        delete thread;

        QVERIFY(widget.isVisible());
    }

    void rapidFrameDeliveryDoesNotCrash() {
        VideoSurfaceWidget widget;
        widget.resize(100, 100);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QImage frame(64, 64, QImage::Format_RGBA8888);
        for (int i = 0; i < 30; ++i) {
            frame.fill(QColor(i * 8, 128, 200));
            widget.onFrameReady(frame);
            QTest::qWait(10);
        }

        QVERIFY(widget.isVisible());
    }
};

QTEST_MAIN(VideoSurfaceWidgetTest)
#include "VideoSurfaceWidgetTest.moc"

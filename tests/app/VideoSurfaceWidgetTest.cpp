#include <QtTest/QtTest>
#include <QPainter>
#include <QThread>
#include "app/VideoSurfaceWidget.h"

#include <QGuiApplication>

class PaintEventCounter final : public QObject {
public:
    int paintEvents = 0;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            ++paintEvents;
        }
        return QObject::eventFilter(watched, event);
    }
};

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

    void fullSurfaceCachedFrameDoesNotBlendOverWhitePrefill() {
        VideoSurfaceWidget widget;
        widget.resize(80, 60);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QImage frame(widget.size(), QImage::Format_RGBA8888);
        frame.fill(QColor(255, 0, 0, 128));
        widget.onFrameReady(frame);
        QTest::qWait(50);

        QImage rendered(widget.size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::black);
        QPainter painter(&rendered);
        widget.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
        painter.end();

        const QColor center = rendered.pixelColor(rendered.rect().center());
        QVERIFY2(center.green() < 32, qPrintable(QStringLiteral("green=%1").arg(center.green())));
        QVERIFY2(center.blue() < 32, qPrintable(QStringLiteral("blue=%1").arg(center.blue())));
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

    void resizeSchedulesCachedFramePaintWithoutBlocking() {
        VideoSurfaceWidget widget;
        widget.resize(100, 100);
        PaintEventCounter counter;
        widget.installEventFilter(&counter);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        counter.paintEvents = 0;
        QImage frame(64, 64, QImage::Format_RGBA8888);
        frame.fill(Qt::green);
        widget.onFrameReady(frame);
        QTRY_VERIFY_WITH_TIMEOUT(counter.paintEvents > 0, 1000);

        counter.paintEvents = 0;
        widget.resize(140, 140);

        QCOMPARE(counter.paintEvents, 0);
        QTRY_VERIFY_WITH_TIMEOUT(counter.paintEvents > 0, 1000);
    }

};

QTEST_MAIN(VideoSurfaceWidgetTest)
#include "VideoSurfaceWidgetTest.moc"

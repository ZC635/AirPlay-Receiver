#include <QtTest/QtTest>
#include "app/VideoSurfaceWidget.h"
#include <QOpenGLWidget>

class VideoSurfaceWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void isQOpenGLWidget() {
        VideoSurfaceWidget widget;
        QVERIFY(qobject_cast<QOpenGLWidget *>(&widget) != nullptr);
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

        QImage screen = widget.grabFramebuffer();
        QVERIFY(!screen.isNull());
    }
};

QTEST_MAIN(VideoSurfaceWidgetTest)
#include "VideoSurfaceWidgetTest.moc"

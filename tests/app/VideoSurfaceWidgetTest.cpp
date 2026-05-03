#include <QtTest/QtTest>
#include "app/VideoSurfaceWidget.h"

#include <QWidget>

class VideoSurfaceWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void hasNativeWindowAttribute() {
        VideoSurfaceWidget widget;
        QVERIFY(widget.testAttribute(Qt::WA_NativeWindow));
        QVERIFY(widget.winId() != 0);
    }

    void hasBlackBackground() {
        VideoSurfaceWidget widget;
        QVERIFY(widget.palette().color(QPalette::Window) == Qt::black);
        QVERIFY(!widget.autoFillBackground());
    }
};

QTEST_MAIN(VideoSurfaceWidgetTest)
#include "VideoSurfaceWidgetTest.moc"

#include <QtTest/QtTest>
#include "app/VideoSurfaceWidget.h"

#include <QImage>
#include <QPainter>
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

    void resetFillsDefaultWindowColor() {
        const QColor expected = QApplication::palette().color(QPalette::Window);
        VideoSurfaceWidget widget;
        widget.resize(100, 100);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        widget.reset();

        QImage image(100, 100, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        widget.render(&painter);
        painter.end();

        QCOMPARE(image.pixelColor(50, 50), expected);
    }
};

QTEST_MAIN(VideoSurfaceWidgetTest)
#include "VideoSurfaceWidgetTest.moc"

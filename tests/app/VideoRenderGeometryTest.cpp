#include <QtTest/QtTest>

#include "app/VideoRenderGeometry.h"

class VideoRenderGeometryTest : public QObject {
    Q_OBJECT

private slots:
    void fillModeReturnsFullBoundsRect() {
        QCOMPARE(videoTargetRect(QSizeF(1920, 1080), QSizeF(800, 600), false),
                 QRectF(QPointF(0, 0), QSizeF(800, 600)));
    }

    void fitModePillarboxesSquareVideoInWideWidget() {
        QCOMPARE(videoTargetRect(QSizeF(100, 100), QSizeF(400, 200), true),
                 QRectF(QPointF(100, 0), QSizeF(200, 200)));
    }

    void fitModeLetterboxesWideVideoInTallWidget() {
        QCOMPARE(videoTargetRect(QSizeF(400, 100), QSizeF(200, 400), true),
                 QRectF(QPointF(0, 175), QSizeF(200, 50)));
    }

    void fitModeFillsBoundsWhenOnlyAspectLockRoundingDiffers() {
        QCOMPARE(videoTargetRect(QSizeF(1920, 1080), QSizeF(1001, 563), true),
                 QRectF(QPointF(0, 0), QSizeF(1001, 563)));
    }

    void emptySourceOrBoundsReturnsEmptyRect() {
        QVERIFY(videoTargetRect(QSizeF(0, 100), QSizeF(200, 200), true).isEmpty());
        QVERIFY(videoTargetRect(QSizeF(100, 0), QSizeF(200, 200), true).isEmpty());
        QVERIFY(videoTargetRect(QSizeF(100, 100), QSizeF(0, 200), true).isEmpty());
        QVERIFY(videoTargetRect(QSizeF(100, 100), QSizeF(200, 0), true).isEmpty());
    }
};

QTEST_MAIN(VideoRenderGeometryTest)
#include "VideoRenderGeometryTest.moc"

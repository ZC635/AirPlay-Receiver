#include <QtTest/QtTest>

#include "backend/RecordingTimeline.h"

#include <limits>

namespace {

constexpr qint64 kSecond = 1'000'000'000;

RecordingVideoDescription description(int width = 1920,
                                      int height = 1080,
                                      int fpsNumerator = 30,
                                      int fpsDenominator = 1,
                                      int parNumerator = 1,
                                      int parDenominator = 1) {
    return {width, height, fpsNumerator, fpsDenominator, parNumerator, parDenominator};
}

qint64 valueOrFail(const std::optional<qint64> &value) {
    if (!value.has_value()) {
        QTest::qFail("Expected a normalized timestamp", __FILE__, __LINE__);
        return -1;
    }
    return *value;
}

void verifyStrictlyIncreasingAtMost(const QVector<qint64> &values, qint64 target) {
    for (qsizetype index = 0; index < values.size(); ++index) {
        QVERIFY(values.at(index) <= target);
        if (index > 0) QVERIFY(values.at(index) > values.at(index - 1));
    }
}

} // namespace

class RecordingTimelineTest : public QObject {
    Q_OBJECT

private slots:
    void operationsBeforeStartReturnNoTimestamps() {
        RecordingTimeline timeline;

        QVERIFY(!timeline.normalizeVideo(10 * kSecond, 20 * kSecond).has_value());
        QVERIFY(!timeline.normalizeAudio(10 * kSecond).has_value());
        QVERIFY(timeline.blackFramePts(20 * kSecond).isEmpty());
    }

    void normalizesBothTracksAgainstFirstVideoSourcePts() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(10 * kSecond, description(), 20 * kSecond, 60));

        QCOMPARE(valueOrFail(timeline.normalizeVideo(10 * kSecond, 20 * kSecond)), qint64(0));
        QVERIFY(!timeline.normalizeVideo(9'999'999'999, 20'100'000'000).has_value());
        QVERIFY(!timeline.normalizeAudio(9'900'000'000).has_value());
        QCOMPARE(valueOrFail(timeline.normalizeAudio(10'400'000'000)), qint64(400'000'000));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(10'500'000'000, 20'500'000'000)),
                 qint64(500'000'000));
        QCOMPARE(valueOrFail(timeline.normalizeAudio(10'600'000'000)), qint64(600'000'000));
    }

    void startUsesFirstVideoArrivalForInitialPauseTimer() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(10 * kSecond, description(), 20 * kSecond, 30));

        QVERIFY(timeline.blackFramePts(20'999'999'999).isEmpty());
        QCOMPARE(timeline.blackFramePts(21 * kSecond), QVector<qint64>{kSecond});
    }

    void startRejectsInvalidOriginOrDimensions() {
        RecordingTimeline timeline;

        QVERIFY(!timeline.start(-1, description(), 0, 30));
        QVERIFY(!timeline.start(0, description(), -1, 30));
        QVERIFY(!timeline.start(0, description(0, 1080), 0, 30));
        QVERIFY(!timeline.start(0, description(1920, 0), 0, 30));
        QVERIFY(!timeline.start(0, description(-1, 1080), 0, 30));
        QVERIFY(!timeline.start(0, description(1920, -1), 0, 30));
        QVERIFY(!timeline.normalizeVideo(0, 0).has_value());
    }

    void validCapsFrameRateWinsAndIsLocked() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(0, description(1920, 1080, 24'000, 1'001), 0, -1));

        const RecordingVideoDescription locked = timeline.lockedDescription();
        QCOMPARE(locked.width, 1920);
        QCOMPARE(locked.height, 1080);
        QCOMPARE(locked.fpsNumerator, 24'000);
        QCOMPARE(locked.fpsDenominator, 1'001);
    }

    void invalidCapsFrameRateUsesOnlySupportedNegotiatedFallback_data() {
        QTest::addColumn<int>("capsNumerator");
        QTest::addColumn<int>("capsDenominator");
        QTest::addColumn<int>("negotiatedFps");

        QTest::newRow("missing-15") << 0 << 1 << 15;
        QTest::newRow("zero-30") << 0 << 0 << 30;
        QTest::newRow("negative-numerator-60") << -1 << 1 << 60;
        QTest::newRow("invalid-denominator-30") << 30 << -1 << 30;
    }

    void invalidCapsFrameRateUsesOnlySupportedNegotiatedFallback() {
        QFETCH(int, capsNumerator);
        QFETCH(int, capsDenominator);
        QFETCH(int, negotiatedFps);
        RecordingTimeline timeline;

        QVERIFY(timeline.start(0, description(1280, 720, capsNumerator, capsDenominator),
                               0, negotiatedFps));
        const RecordingVideoDescription locked = timeline.lockedDescription();
        QCOMPARE(locked.fpsNumerator, negotiatedFps);
        QCOMPARE(locked.fpsDenominator, 1);
    }

    void invalidCapsAndUnsupportedNegotiatedFrameRateFail_data() {
        QTest::addColumn<int>("negotiatedFps");
        QTest::newRow("zero") << 0;
        QTest::newRow("negative") << -1;
        QTest::newRow("24") << 24;
        QTest::newRow("29") << 29;
        QTest::newRow("120") << 120;
    }

    void invalidCapsAndUnsupportedNegotiatedFrameRateFail() {
        QFETCH(int, negotiatedFps);
        RecordingTimeline timeline;

        QVERIFY(!timeline.start(0, description(1920, 1080, 0, 0), 0, negotiatedFps));
        QVERIFY(!timeline.normalizeVideo(0, 0).has_value());
    }

    void repeatedStartCompletelyResetsRecordingState() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(10 * kSecond, description(), 20 * kSecond, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(10 * kSecond, 20 * kSecond)), qint64(0));
        QCOMPARE(timeline.blackFramePts(21 * kSecond), QVector<qint64>{kSecond});

        QVERIFY(timeline.start(30 * kSecond, description(1280, 720, 0, 0, 4, 3),
                               40 * kSecond, 15));

        QVERIFY(!timeline.normalizeAudio(29'999'999'999).has_value());
        QCOMPARE(valueOrFail(timeline.normalizeVideo(30 * kSecond, 40 * kSecond)), qint64(0));
        QVERIFY(timeline.blackFramePts(40'999'999'999).isEmpty());
        QCOMPARE(timeline.blackFramePts(41 * kSecond), QVector<qint64>{kSecond});
        const RecordingVideoDescription locked = timeline.lockedDescription();
        QCOMPARE(locked.width, 1280);
        QCOMPARE(locked.height, 720);
        QCOMPARE(locked.fpsNumerator, 15);
        QCOMPARE(locked.fpsDenominator, 1);
        QCOMPARE(locked.pixelAspectNumerator, 1);
        QCOMPARE(locked.pixelAspectDenominator, 1);
    }

    void locksSquarePixelsAndOnlyDimensionsCountAsChange() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(0, description(1080, 1920, 60, 1, 4, 3), 0, 30));

        const RecordingVideoDescription locked = timeline.lockedDescription();
        QCOMPARE(locked.pixelAspectNumerator, 1);
        QCOMPARE(locked.pixelAspectDenominator, 1);
        QVERIFY(!timeline.dimensionsChanged(description(1080, 1920, 15, 1, 100, 7)));
        QVERIFY(timeline.dimensionsChanged(description(1081, 1920, 60, 1, 1, 1)));
        QVERIFY(timeline.dimensionsChanged(description(1080, 1919, 60, 1, 1, 1)));
        QCOMPARE(locked.width, 1080);
        QCOMPARE(locked.height, 1920);
    }

    void bitratePolicy_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<double>("fps");
        QTest::addColumn<int>("expected");

        QTest::newRow("invalid-width") << 0 << 1080 << 30.0 << 0;
        QTest::newRow("invalid-height") << 1920 << -1 << 30.0 << 0;
        QTest::newRow("invalid-zero-fps") << 1920 << 1080 << 0.0 << 0;
        QTest::newRow("invalid-negative-fps") << 1920 << 1080 << -1.0 << 0;
        QTest::newRow("invalid-nan-fps") << 1920 << 1080
                                         << std::numeric_limits<double>::quiet_NaN() << 0;
        QTest::newRow("invalid-infinite-fps") << 1920 << 1080
                                                << std::numeric_limits<double>::infinity() << 0;
        QTest::newRow("540p-15") << 960 << 540 << 15.0 << 4'000'000;
        QTest::newRow("540p-30-boundary") << 960 << 540 << 30.0 << 4'000'000;
        QTest::newRow("540p-60") << 960 << 540 << 60.0 << 6'000'000;
        QTest::newRow("portrait-540p-60") << 540 << 960 << 60.0 << 6'000'000;
        QTest::newRow("541p-30") << 960 << 541 << 30.0 << 6'000'000;
        QTest::newRow("720p-15") << 1280 << 720 << 15.0 << 6'000'000;
        QTest::newRow("720p-30-boundary") << 1280 << 720 << 30.0 << 6'000'000;
        QTest::newRow("720p-60") << 1280 << 720 << 60.0 << 9'000'000;
        QTest::newRow("portrait-720p-60") << 720 << 1280 << 60.0 << 9'000'000;
        QTest::newRow("721p-30") << 1280 << 721 << 30.0 << 10'000'000;
        QTest::newRow("1080p-15") << 1920 << 1080 << 15.0 << 10'000'000;
        QTest::newRow("1080p-30-boundary") << 1920 << 1080 << 30.0 << 10'000'000;
        QTest::newRow("1080p-60") << 1920 << 1080 << 60.0 << 15'000'000;
        QTest::newRow("portrait-1080p-60") << 1080 << 1920 << 60.0 << 15'000'000;
    }

    void bitratePolicy() {
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(double, fps);
        QFETCH(int, expected);

        QCOMPARE(recordingVideoBitrateBitsPerSecond(width, height, fps), expected);
    }

    void blackFramesBeginAfterOneSecondAndFollowThirtyFpsCadence() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(10 * kSecond, description(), 0, 60));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(10 * kSecond, 0)), qint64(0));

        QVERIFY(timeline.blackFramePts(999'999'999).isEmpty());
        QCOMPARE(timeline.blackFramePts(kSecond), QVector<qint64>{kSecond});
        const QVector<qint64> catchUp = timeline.blackFramePts(1'100'000'000);
        QCOMPARE(catchUp, QVector<qint64>({1'033'333'333, 1'066'666'666, 1'099'999'999}));
        verifyStrictlyIncreasingAtMost(catchUp, 1'100'000'000);
    }

    void firstLatePollIncludesTimeoutFrameAndCadenceCatchUp() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(0, description(), 0, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(0, 0)), qint64(0));

        const QVector<qint64> catchUp = timeline.blackFramePts(1'100'000'000);

        QCOMPARE(catchUp, QVector<qint64>({1'000'000'000, 1'033'333'333,
                                          1'066'666'666, 1'099'999'999}));
        verifyStrictlyIncreasingAtMost(catchUp, 1'100'000'000);
    }

    void everyRealSampleStopsBlackAndResetsTimeoutEvenWhenPtsIsDropped() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(0, description(), 0, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(0, 0)), qint64(0));
        const QVector<qint64> initialBlack = timeline.blackFramePts(1'100'000'000);
        QCOMPARE(initialBlack.constLast(), qint64(1'099'999'999));

        QVERIFY(!timeline.normalizeVideo(500'000'000, 1'200'000'000).has_value());
        QVERIFY(timeline.blackFramePts(2'199'999'999).isEmpty());
        QCOMPARE(timeline.blackFramePts(2'200'000'000), QVector<qint64>{2'099'999'999});
        QVERIFY(!timeline.normalizeVideo(2'099'999'999, 2'210'000'000).has_value());
        QCOMPARE(valueOrFail(timeline.normalizeVideo(2'200'000'000, 2'220'000'000)),
                 qint64(2'200'000'000));
    }

    void realSampleBeforeOriginStillResetsBlackTimeout() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(10 * kSecond, description(), 0, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(10 * kSecond, 0)), qint64(0));
        const QVector<qint64> initialBlack = timeline.blackFramePts(1'100'000'000);
        QCOMPARE(initialBlack.constLast(), qint64(1'099'999'999));

        QVERIFY(!timeline.normalizeVideo(9'900'000'000, 1'200'000'000).has_value());

        QVERIFY(timeline.blackFramePts(2'199'999'999).isEmpty());
        QCOMPARE(timeline.blackFramePts(2'200'000'000), QVector<qint64>{2'099'999'999});
    }

    void backwardArrivalAndBackwardPollDoNotCorruptTimers() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(10 * kSecond, description(), 10 * kSecond, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(10 * kSecond, 10 * kSecond)), qint64(0));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(10'100'000'000, 10'100'000'000)),
                 qint64(100'000'000));

        QVERIFY(!timeline.normalizeVideo(10'200'000'000, 10'050'000'000).has_value());
        QVERIFY(timeline.blackFramePts(11'060'000'000).isEmpty());
        QCOMPARE(timeline.blackFramePts(11'100'000'000), QVector<qint64>{1'100'000'000});
        QVERIFY(timeline.blackFramePts(11'000'000'000).isEmpty());
        QCOMPARE(timeline.blackFramePts(11'133'333'333), QVector<qint64>{1'133'333'333});
    }

    void blackCadenceUsesLockedFifteenAndSixtyFps_data() {
        QTest::addColumn<int>("fps");
        QTest::addColumn<qint64>("poll");
        QTest::addColumn<QVector<qint64>>("expected");

        QTest::newRow("15fps") << 15 << qint64(1'200'000'000)
                                << QVector<qint64>({1'000'000'000, 1'066'666'666,
                                                   1'133'333'332, 1'199'999'998});
        QTest::newRow("60fps") << 60 << qint64(1'050'000'000)
                                << QVector<qint64>({1'000'000'000, 1'016'666'666,
                                                   1'033'333'332, 1'049'999'998});
    }

    void blackCadenceUsesLockedFifteenAndSixtyFps() {
        QFETCH(int, fps);
        QFETCH(qint64, poll);
        QFETCH(QVector<qint64>, expected);
        RecordingTimeline timeline;
        QVERIFY(timeline.start(0, description(1920, 1080, fps, 1), 0, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(0, 0)), qint64(0));

        const QVector<qint64> actual = timeline.blackFramePts(poll);

        QCOMPARE(actual, expected);
        verifyStrictlyIncreasingAtMost(actual, poll);
    }

    void longPollIsBoundedAndRepeatedSamePollContinuesCatchUp() {
        RecordingTimeline timeline;
        QVERIFY(timeline.start(0, description(1920, 1080, 60, 1), 0, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(0, 0)), qint64(0));
        const qint64 target = 100 * kSecond;

        const QVector<qint64> first = timeline.blackFramePts(target);
        const QVector<qint64> second = timeline.blackFramePts(target);

        QCOMPARE(first.size(), 120);
        QCOMPARE(second.size(), 120);
        verifyStrictlyIncreasingAtMost(first, target);
        verifyStrictlyIncreasingAtMost(second, target);
        QVERIFY(second.constFirst() > first.constLast());
    }

    void timestampArithmeticDoesNotOverflow() {
        RecordingTimeline timeline;
        const qint64 maximum = std::numeric_limits<qint64>::max();
        QVERIFY(timeline.start(0, description(), 0, 30));
        QCOMPARE(valueOrFail(timeline.normalizeVideo(maximum - 500'000'000, 0)),
                 maximum - 500'000'000);

        QVERIFY(timeline.blackFramePts(maximum).isEmpty());
        QVERIFY(!timeline.normalizeVideo(maximum - 500'000'001, 1).has_value());
        QVERIFY(!timeline.normalizeAudio(-1).has_value());
    }
};

QTEST_APPLESS_MAIN(RecordingTimelineTest)
#include "RecordingTimelineTest.moc"

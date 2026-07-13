#pragma once

#include <QVector>
#include <QtGlobal>

#include <optional>

struct RecordingVideoDescription {
    int width = 0;
    int height = 0;
    int fpsNumerator = 0;
    int fpsDenominator = 1;
    int pixelAspectNumerator = 1;
    int pixelAspectDenominator = 1;
};

class RecordingTimeline {
public:
    bool start(qint64 firstPts,
               RecordingVideoDescription description,
               qint64 firstArrival,
               int negotiatedFps);
    std::optional<qint64> normalizeVideo(qint64 sourcePts, qint64 arrival);
    std::optional<qint64> normalizeAudio(qint64 sourcePts) const;
    QVector<qint64> blackFramePts(qint64 now);
    bool dimensionsChanged(const RecordingVideoDescription &description) const;
    RecordingVideoDescription lockedDescription() const;

private:
    void reset();
    void scheduleBlackAfterReal(qint64 normalizedPts);

    bool m_started = false;
    qint64 m_originPts = 0;
    RecordingVideoDescription m_lockedDescription;
    qint64 m_frameDurationQuotient = 0;
    qint64 m_frameDurationRemainder = 0;
    qint64 m_frameDurationDivisor = 1;
    qint64 m_frameRemainderAccumulator = 0;
    std::optional<qint64> m_lastEmittedPts;
    std::optional<qint64> m_lastRealArrival;
    std::optional<qint64> m_blackAnchorPts;
    std::optional<qint64> m_nextBlackPts;
    std::optional<qint64> m_lastBlackPoll;
};

int recordingVideoBitrateBitsPerSecond(int width, int height, double fps);

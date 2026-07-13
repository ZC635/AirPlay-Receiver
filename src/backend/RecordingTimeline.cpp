#include "backend/RecordingTimeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr qint64 kNanosecondsPerSecond = 1'000'000'000;
constexpr int kMaximumBlackFramesPerPoll = 120;

bool addWithoutOverflow(qint64 left, qint64 right, qint64 *result) {
    if (right > 0 && left > std::numeric_limits<qint64>::max() - right) return false;
    if (right < 0 && left < std::numeric_limits<qint64>::min() - right) return false;
    *result = left + right;
    return true;
}

qint64 saturatedAdd(qint64 left, qint64 right) {
    qint64 result = 0;
    if (addWithoutOverflow(left, right, &result)) return result;
    return right >= 0 ? std::numeric_limits<qint64>::max()
                      : std::numeric_limits<qint64>::min();
}

bool supportedNegotiatedFps(int fps) {
    return fps == 15 || fps == 30 || fps == 60;
}

} // namespace

void RecordingTimeline::reset() {
    m_started = false;
    m_originPts = 0;
    m_lockedDescription = {};
    m_frameDuration = 0;
    m_lastEmittedPts.reset();
    m_lastRealArrival.reset();
    m_blackAnchorPts.reset();
    m_nextBlackPts.reset();
    m_lastBlackPoll.reset();
}

bool RecordingTimeline::start(qint64 firstPts,
                              RecordingVideoDescription description,
                              qint64 firstArrival,
                              int negotiatedFps) {
    reset();
    if (firstPts < 0 || firstArrival < 0 || description.width <= 0 || description.height <= 0) {
        return false;
    }

    if (description.fpsNumerator <= 0 || description.fpsDenominator <= 0) {
        if (!supportedNegotiatedFps(negotiatedFps)) return false;
        description.fpsNumerator = negotiatedFps;
        description.fpsDenominator = 1;
    }
    description.pixelAspectNumerator = 1;
    description.pixelAspectDenominator = 1;

    const qint64 numerator = qint64(kNanosecondsPerSecond) * description.fpsDenominator;
    m_frameDuration = std::max<qint64>(1, numerator / description.fpsNumerator);
    m_originPts = firstPts;
    m_lockedDescription = description;
    m_lastRealArrival = firstArrival;
    scheduleBlackAfterReal(0);
    m_started = true;
    return true;
}

std::optional<qint64> RecordingTimeline::normalizeVideo(qint64 sourcePts, qint64 arrival) {
    if (!m_started || sourcePts < 0 || arrival < 0) return std::nullopt;
    if (m_lastRealArrival.has_value() && arrival < *m_lastRealArrival) return std::nullopt;

    const qint64 normalizedPts = sourcePts - m_originPts;
    m_lastRealArrival = arrival;
    m_lastBlackPoll.reset();
    scheduleBlackAfterReal(m_lastEmittedPts.has_value()
                               ? std::max(normalizedPts, *m_lastEmittedPts)
                               : std::max<qint64>(normalizedPts, 0));

    if (normalizedPts < 0) return std::nullopt;
    if (m_lastEmittedPts.has_value() && normalizedPts <= *m_lastEmittedPts) return std::nullopt;
    m_lastEmittedPts = normalizedPts;
    return normalizedPts;
}

std::optional<qint64> RecordingTimeline::normalizeAudio(qint64 sourcePts) const {
    if (!m_started || sourcePts < m_originPts) return std::nullopt;
    return sourcePts - m_originPts;
}

void RecordingTimeline::scheduleBlackAfterReal(qint64 normalizedPts) {
    m_blackAnchorPts = normalizedPts;
    qint64 next = 0;
    if (addWithoutOverflow(normalizedPts, kNanosecondsPerSecond, &next)) {
        m_nextBlackPts = next;
    } else {
        m_nextBlackPts.reset();
    }
}

QVector<qint64> RecordingTimeline::blackFramePts(qint64 now) {
    QVector<qint64> result;
    if (!m_started || now < 0 || !m_lastRealArrival.has_value() ||
        !m_blackAnchorPts.has_value() || !m_nextBlackPts.has_value()) {
        return result;
    }
    if (m_lastBlackPoll.has_value() && now < *m_lastBlackPoll) return result;
    if (now < *m_lastRealArrival) return result;
    m_lastBlackPoll = now;

    const qint64 elapsed = now - *m_lastRealArrival;
    if (elapsed < kNanosecondsPerSecond) return result;
    const qint64 target = saturatedAdd(*m_blackAnchorPts, elapsed);

    result.reserve(kMaximumBlackFramesPerPoll);
    while (result.size() < kMaximumBlackFramesPerPoll &&
           m_nextBlackPts.has_value() && *m_nextBlackPts <= target) {
        const qint64 current = *m_nextBlackPts;
        if (!m_lastEmittedPts.has_value() || current > *m_lastEmittedPts) {
            result.append(current);
            m_lastEmittedPts = current;
        }
        qint64 next = 0;
        if (addWithoutOverflow(current, m_frameDuration, &next)) {
            m_nextBlackPts = next;
        } else {
            m_nextBlackPts.reset();
        }
    }
    return result;
}

bool RecordingTimeline::dimensionsChanged(const RecordingVideoDescription &description) const {
    return description.width != m_lockedDescription.width ||
           description.height != m_lockedDescription.height;
}

RecordingVideoDescription RecordingTimeline::lockedDescription() const {
    return m_lockedDescription;
}

int recordingVideoBitrateBitsPerSecond(int width, int height, double fps) {
    if (width <= 0 || height <= 0 || !std::isfinite(fps) || fps <= 0.0) return 0;
    const int tier = std::min(width, height);
    const bool highFrameRate = fps > 30.0;
    if (tier <= 540) return highFrameRate ? 6'000'000 : 4'000'000;
    if (tier <= 720) return highFrameRate ? 9'000'000 : 6'000'000;
    return highFrameRate ? 15'000'000 : 10'000'000;
}

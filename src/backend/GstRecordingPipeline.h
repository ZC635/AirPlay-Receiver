#pragma once

#include "backend/RecordingTimeline.h"

#include <QString>

#include <gst/gst.h>

#include <atomic>
#include <functional>
#include <memory>

struct GstRecordingPipelineConfig {
    QString videoSpoolPath;
    QString audioSpoolPath;
    QString temporaryMp4Path;
    RecordingVideoDescription video;
    int videoBitrateBitsPerSecond = 0;
    QString preferredEncoder;
};

struct GstRecordingCapabilityResult {
    bool available = false;
    QString preferredEncoder;
    QString error;
};

struct GstRecordingFinalizeResult {
    bool success = false;
    QString error;
};

struct GstRecordingPipelineHooks {
    std::function<bool(const QString &)> factoryExists;
    std::function<bool(const QString &, QString *)> factoryReady;
    std::function<bool(const QString &, QString *)> encoderBuildAllowed;
    std::function<bool(const QString &, const QString &, QString *)> encoderStageAllowed;
    std::function<void(const QString &)> encoderAttempted;
    std::function<void(const QString &)> elementCreated;
    std::function<void(const QString &, const QString &, const QString &)> configurationObserved;
    std::function<void(const QString &, GstBuffer *)> bufferPushed;
    std::function<GstMessage *(GstBus *, GstClockTime)> busTimedPop;
    std::function<qint64()> monotonicMilliseconds;
};

class GstRecordingPipeline {
public:
    explicit GstRecordingPipeline(GstRecordingPipelineHooks hooks = {});
    ~GstRecordingPipeline();

    GstRecordingPipeline(const GstRecordingPipeline &) = delete;
    GstRecordingPipeline &operator=(const GstRecordingPipeline &) = delete;
    GstRecordingPipeline(GstRecordingPipeline &&) = delete;
    GstRecordingPipeline &operator=(GstRecordingPipeline &&) = delete;

    static GstRecordingCapabilityResult probeCapabilities();
    static GstRecordingCapabilityResult probeCapabilities(const GstRecordingPipelineHooks &hooks);

    bool start(const GstRecordingPipelineConfig &config,
               GstSample *firstVideoSample,
               QString *error);
    bool pushVideo(GstSample *sample, qint64 normalizedPtsNs, QString *error);
    bool pushBlackFrame(qint64 normalizedPtsNs, QString *error);
    bool pushAudio(GstSample *sample, qint64 normalizedPtsNs, QString *error);
    GstRecordingFinalizeResult finalize(int deadlineMs, const std::atomic_bool &cancelled);
    void abort();
    QString encoderFactoryName() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

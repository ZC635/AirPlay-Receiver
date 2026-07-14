#pragma once

#include "backend/GstRecordingPipeline.h"
#include "backend/RecordingFileTransaction.h"
#include "backend/RecordingTypes.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QUuid>

#include <atomic>
#include <functional>
#include <memory>

struct RecordingPipelineSession {
    std::function<bool(const GstRecordingPipelineConfig &, GstSample *, QString *)> start;
    std::function<bool(GstSample *, qint64, QString *)> pushVideo;
    std::function<bool(GstSample *, qint64, QString *)> pushAudio;
    std::function<bool(qint64, QString *)> pushBlackFrame;
    std::function<GstRecordingFinalizeResult(int, const std::atomic_bool &)> finalize;
    std::function<void()> abort;
};

struct RecordingControllerHooks {
    std::function<GstRecordingCapabilityResult()> probeCapabilities;
    std::function<QString(const QString &)> ensureOutputDirectory;
    std::function<QStringList(const QString &)> cleanupStaleTemporaryFiles;
    std::function<RecordingFileReservationResult(
        const QString &, const QDateTime &, const QUuid &)> reserve;
    std::function<QString(const RecordingFileReservation &)> commit;
    std::function<void(const RecordingFileReservation &)> discard;
    std::function<QDateTime()> now;
    std::function<QUuid()> uuid;
    std::function<qint64()> monotonicNanoseconds;
    std::function<void()> drainInvocationScheduled;
    std::function<void()> workerDrainCompleted;
    std::function<void()> workerFinalizeEntered;
    std::function<RecordingPipelineSession()> createPipeline;
    std::function<bool(const QString &)> removeOwnedFinal;
};

class RecordingWorker;
class RecordingControllerPrivate;

class RecordingController final : public QObject {
    Q_OBJECT

public:
    static constexpr qsizetype VideoQueueCapacity = 8;
    static constexpr qsizetype AudioQueueCapacity = 64;
    static constexpr int FinalizeDeadlineMilliseconds = 30'000;

    explicit RecordingController(
        int negotiatedFrameRate = 30,
        RecordingControllerHooks hooks = {},
        QObject *parent = nullptr);
    ~RecordingController() override;

    RecordingController(const RecordingController &) = delete;
    RecordingController &operator=(const RecordingController &) = delete;

    bool available() const;
    RecordingState state() const;
    RecordingStartResult start(const RecordingOptions &options);
    void stop();
    void discard();
    bool tryEnqueueVideoSample(GstSample *borrowedSample) noexcept;
    bool tryEnqueueAudioSample(GstSample *borrowedSample) noexcept;
    void sessionEnded(bool canFinalize);

signals:
    void availabilityChanged(bool available);
    void stateChanged(RecordingState state);
    void finished(RecordingResult result);
    void failed(QString error);

private:
    friend class RecordingWorker;
    void setAvailabilityFromAnyThread(bool available);
    void handleWorkerFinalizing(quint64 generation, const QString &warning);
    void handleWorkerFinished(quint64 generation, const RecordingResult &result);
    void handleWorkerFailed(quint64 generation, const QString &error);

    std::unique_ptr<RecordingControllerPrivate> d;
};

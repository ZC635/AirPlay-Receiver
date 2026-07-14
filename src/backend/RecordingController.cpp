#include "backend/RecordingController.h"

#include "backend/GstSampleQueue.h"
#include "backend/RecordingTimeline.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <gst/gst.h>

#include <chrono>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

namespace {

QString exceptionError(const QString &operation)
{
    try {
        throw;
    } catch (const std::exception &exception) {
        return QStringLiteral("%1 failed: %2").arg(operation,
                                                   QString::fromUtf8(exception.what()));
    } catch (...) {
        return QStringLiteral("%1 failed with an unknown exception").arg(operation);
    }
}

bool parseVideoDescription(GstSample *sample, RecordingVideoDescription *description)
{
    if (!sample || !description) return false;
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps) || !buffer ||
        !GST_BUFFER_PTS_IS_VALID(buffer) || GST_BUFFER_PTS(buffer) > G_MAXINT64) {
        return false;
    }
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    if (!structure ||
        g_strcmp0(gst_structure_get_name(structure), "video/x-raw") != 0) {
        return false;
    }
    const char *format = gst_structure_get_string(structure, "format");
    if (!format || g_strcmp0(format, "RGBA") != 0) return false;

    RecordingVideoDescription parsed;
    if (!gst_structure_get_int(structure, "width", &parsed.width) ||
        !gst_structure_get_int(structure, "height", &parsed.height) ||
        parsed.width <= 0 || parsed.height <= 0) {
        return false;
    }
    const quint64 minimumSize = static_cast<quint64>(parsed.width) *
                                static_cast<quint64>(parsed.height) * 4u;
    if (minimumSize > G_MAXSIZE || gst_buffer_get_size(buffer) < minimumSize) {
        return false;
    }
    if (!gst_structure_get_fraction(structure, "framerate", &parsed.fpsNumerator,
                                    &parsed.fpsDenominator)) {
        parsed.fpsNumerator = 0;
        parsed.fpsDenominator = 1;
    }
    if (!gst_structure_get_fraction(structure, "pixel-aspect-ratio",
                                    &parsed.pixelAspectNumerator,
                                    &parsed.pixelAspectDenominator)) {
        parsed.pixelAspectNumerator = 1;
        parsed.pixelAspectDenominator = 1;
    }
    *description = parsed;
    return true;
}

bool isAudioSample(GstSample *sample)
{
    if (!sample) return false;
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps) || !buffer ||
        !GST_BUFFER_PTS_IS_VALID(buffer) || GST_BUFFER_PTS(buffer) > G_MAXINT64) {
        return false;
    }
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    return structure &&
           g_strcmp0(gst_structure_get_name(structure), "audio/x-raw") == 0;
}

qint64 samplePts(GstSample *sample)
{
    GstBuffer *buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    return buffer && GST_BUFFER_PTS_IS_VALID(buffer) && GST_BUFFER_PTS(buffer) <= G_MAXINT64
        ? static_cast<qint64>(GST_BUFFER_PTS(buffer)) : -1;
}

QString ensureOutputDirectory(const QString &directory)
{
    if (directory.trimmed().isEmpty()) {
        return QStringLiteral("Recording output directory is empty");
    }
    const QString absolutePath = QDir(directory).absolutePath();
    if (!QDir().mkpath(absolutePath) || !QFileInfo(absolutePath).isDir()) {
        return QStringLiteral("Could not create recording output directory \"%1\"")
            .arg(absolutePath);
    }
    if (!QFileInfo(absolutePath).isWritable()) {
        return QStringLiteral("Recording output directory is not writable: \"%1\"")
            .arg(absolutePath);
    }
    return {};
}

RecordingPipelineSession realPipelineSession()
{
    auto pipeline = std::make_shared<GstRecordingPipeline>();
    RecordingPipelineSession session;
    session.start = [pipeline](const GstRecordingPipelineConfig &config,
                               GstSample *sample, QString *error) {
        return pipeline->start(config, sample, error);
    };
    session.pushVideo = [pipeline](GstSample *sample, qint64 pts, QString *error) {
        return pipeline->pushVideo(sample, pts, error);
    };
    session.pushAudio = [pipeline](GstSample *sample, qint64 pts, QString *error) {
        return pipeline->pushAudio(sample, pts, error);
    };
    session.pushBlackFrame = [pipeline](qint64 pts, QString *error) {
        return pipeline->pushBlackFrame(pts, error);
    };
    session.finalize = [pipeline](int deadline, const std::atomic_bool &cancelled) {
        return pipeline->finalize(deadline, cancelled);
    };
    session.abort = [pipeline] { pipeline->abort(); };
    return session;
}

RecordingControllerHooks completedHooks(RecordingControllerHooks hooks)
{
    if (!hooks.probeCapabilities) {
        hooks.probeCapabilities = [] { return GstRecordingPipeline::probeCapabilities(); };
    }
    if (!hooks.ensureOutputDirectory) hooks.ensureOutputDirectory = ensureOutputDirectory;
    if (!hooks.cleanupStaleTemporaryFiles) {
        hooks.cleanupStaleTemporaryFiles = RecordingFileTransaction::cleanupStaleTemporaryFiles;
    }
    if (!hooks.reserve) hooks.reserve = RecordingFileTransaction::reserve;
    if (!hooks.commit) hooks.commit = RecordingFileTransaction::commit;
    if (!hooks.discard) hooks.discard = RecordingFileTransaction::discard;
    if (!hooks.now) hooks.now = [] { return QDateTime::currentDateTime(); };
    if (!hooks.uuid) hooks.uuid = [] { return QUuid::createUuid(); };
    if (!hooks.monotonicNanoseconds) {
        hooks.monotonicNanoseconds = [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
    }
    if (!hooks.createPipeline) hooks.createPipeline = realPipelineSession;
    if (!hooks.removeOwnedFinal) {
        hooks.removeOwnedFinal = [](const QString &path) {
            return path.isEmpty() || !QFileInfo::exists(path) || QFile::remove(path);
        };
    }
    return hooks;
}

} // namespace

class RecordingControllerPrivate;

struct VideoEnvelope {
    GstSample *sample = nullptr;
    qint64 arrivalNanoseconds = -1;
};

class VideoEnvelopeQueue final {
public:
    explicit VideoEnvelopeQueue(qsizetype capacity)
        : m_samples(capacity)
    {
    }

    bool tryPushBorrowed(GstSample *sample, qint64 arrivalNanoseconds) noexcept
    {
        if (!sample || arrivalNanoseconds < 0) return false;
        QMutexLocker locker(&m_mutex);
        try {
            m_arrivals.push_back(arrivalNanoseconds);
        } catch (...) {
            return false;
        }
        if (!m_samples.tryPushBorrowed(sample)) {
            m_arrivals.pop_back();
            return false;
        }
        return true;
    }

    VideoEnvelope tryPopOwned()
    {
        QMutexLocker locker(&m_mutex);
        GstSample *sample = m_samples.tryPopOwned();
        if (!sample) return {};
        const qint64 arrival = m_arrivals.front();
        m_arrivals.pop_front();
        return {sample, arrival};
    }

    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_samples.clear();
        m_arrivals.clear();
    }

    qsizetype size() const
    {
        QMutexLocker locker(&m_mutex);
        return static_cast<qsizetype>(m_arrivals.size());
    }

private:
    mutable QMutex m_mutex;
    GstSampleQueue m_samples;
    std::deque<qint64> m_arrivals;
};

class RecordingWorker final : public QObject {
public:
    RecordingWorker(RecordingController *owner,
                    VideoEnvelopeQueue *videoQueue,
                    GstSampleQueue *audioQueue,
                    QMutex *queueGate,
                    std::atomic_bool *drainScheduled,
                    std::atomic_bool *accepting,
                    std::atomic_bool *cancelled,
                    RecordingControllerHooks hooks)
        : m_owner(owner),
          m_videoQueue(videoQueue),
          m_audioQueue(audioQueue),
          m_queueGate(queueGate),
          m_drainScheduled(drainScheduled),
          m_accepting(accepting),
          m_cancelled(cancelled),
          m_hooks(std::move(hooks))
    {
    }

    GstRecordingCapabilityResult probeCapabilities()
    {
        try {
            return m_hooks.probeCapabilities();
        } catch (...) {
            return {false, {},
                    exceptionError(QStringLiteral("Recording capability probe"))};
        }
    }

    QString begin(const RecordingFileReservation &reservation,
                  const QString &preferredEncoder,
                  int negotiatedFrameRate,
                  quint64 generation)
    {
        if (!removeExplicitOwnedFinal()) {
            try { m_hooks.discard(reservation); } catch (...) {}
            return QStringLiteral("Could not remove a previously owned recording file");
        }
        cleanupPipeline();
        clearQueues();
        m_reservation = reservation;
        m_preferredEncoder = preferredEncoder;
        m_negotiatedFrameRate = negotiatedFrameRate;
        m_generation = generation;
        m_timeline = RecordingTimeline{};
        m_pipelineStarted = false;
        m_active = true;
        m_finalizing = false;
        m_warning.clear();
        if (!m_blackTimer) {
            m_blackTimer = new QTimer(this);
            m_blackTimer->setInterval(20);
            m_blackTimer->setTimerType(Qt::PreciseTimer);
            QObject::connect(m_blackTimer, &QTimer::timeout, this,
                             [this] { pollBlackFrames(); });
        }
        m_blackTimer->start();
        try {
            m_pipeline = m_hooks.createPipeline();
        } catch (...) {
            const QString error = exceptionError(QStringLiteral("Recording pipeline factory"));
            try { m_hooks.discard(m_reservation); } catch (...) {}
            resetSession();
            return error;
        }
        if (!m_pipeline.start || !m_pipeline.pushVideo || !m_pipeline.pushAudio ||
            !m_pipeline.pushBlackFrame || !m_pipeline.finalize || !m_pipeline.abort) {
            try { m_hooks.discard(m_reservation); } catch (...) {}
            resetSession();
            return QStringLiteral("Recording pipeline factory returned an incomplete session");
        }
        return {};
    }

    void drain()
    {
        if (!m_active || m_finalizing) {
            clearQueues();
            finishDrainScheduling();
            return;
        }

        bool dimensionBoundary = false;
        int processed = 0;
        while (m_active && !m_finalizing && processed < DrainBatchSize) {
            bool madeProgress = false;
            VideoEnvelope video = m_videoQueue->tryPopOwned();
            if (video.sample) {
                dimensionBoundary = processVideo(video.sample,
                                                 video.arrivalNanoseconds);
                gst_sample_unref(video.sample);
                ++processed;
                madeProgress = true;
                if (dimensionBoundary || !m_active || m_finalizing ||
                    processed >= DrainBatchSize) {
                    break;
                }
            }

            GstSample *audio = m_audioQueue->tryPopOwned();
            if (audio) {
                processAudio(audio);
                gst_sample_unref(audio);
                ++processed;
                madeProgress = true;
            }
            if (!madeProgress) break;
        }
        if (dimensionBoundary && m_active && !m_finalizing) {
            clearQueues();
            finalizeActiveSession();
        }
        finishDrainScheduling();
    }

    void finalize(quint64 generation, QString warning = {})
    {
        if (!m_active || m_finalizing || generation != m_generation) return;
        try {
            if (m_hooks.workerFinalizeEntered) m_hooks.workerFinalizeEntered();
        } catch (...) {
        }
        m_warning = std::move(warning);
        drainQueuedSnapshot();
        if (!m_active || m_finalizing) return;
        finalizeActiveSession();
    }

private:
    static constexpr int DrainBatchSize = 4;

    void finalizeActiveSession()
    {
        if (!m_active || m_finalizing) return;
        m_finalizing = true;
        clearQueues();

        if (!m_pipelineStarted) {
            failSession(QStringLiteral("No valid video sample arrived after recording Start"));
            return;
        }

        GstRecordingFinalizeResult result;
        try {
            result = m_pipeline.finalize(
                RecordingController::FinalizeDeadlineMilliseconds, *m_cancelled);
        } catch (...) {
            result = {false, exceptionError(QStringLiteral("Recording finalization"))};
        }
        if (m_cancelled->load(std::memory_order_acquire)) {
            discardOwnedSession();
            return;
        }
        if (!result.success) {
            failSession(result.error.isEmpty()
                            ? QStringLiteral("Recording finalization failed") : result.error);
            return;
        }

        QString commitError;
        try {
            commitError = m_hooks.commit(m_reservation);
        } catch (...) {
            commitError = exceptionError(QStringLiteral("Recording commit"));
        }
        if (!commitError.isEmpty()) {
            failSession(commitError);
            return;
        }
        m_ownedFinal = m_reservation.finalPath;
        if (m_cancelled->load(std::memory_order_acquire)) {
            removeExplicitOwnedFinal();
            resetSession();
            return;
        }

        const RecordingResult recordingResult{m_ownedFinal, m_warning};
        const quint64 completedGeneration = m_generation;
        resetSession();
        if (m_owner) {
            QPointer<RecordingController> owner = m_owner;
            QMetaObject::invokeMethod(owner, [owner, completedGeneration, recordingResult] {
                if (owner) owner->handleWorkerFinished(completedGeneration, recordingResult);
            }, Qt::QueuedConnection);
        }
    }

public:
    void discardSynchronously(quint64 generation)
    {
        if (generation != m_generation && !m_active && m_ownedFinal.isEmpty()) {
            clearQueues();
            return;
        }
        discardOwnedSession();
    }

    void failUnfinalizable(quint64 generation, const QString &error)
    {
        if (!m_active || generation != m_generation) return;
        m_finalizing = true;
        failSession(error);
    }

    void relinquishCommittedFinal(quint64 generation, const QString &path)
    {
        if (generation == m_generation && !m_active && m_ownedFinal == path) {
            m_ownedFinal.clear();
        }
    }

    void shutdown()
    {
        discardOwnedSession();
        clearQueues();
    }

private:
    void pollBlackFrames()
    {
        if (!m_active || m_finalizing || !m_pipelineStarted) return;
        if (m_videoQueue->size() > 0) return;
        qint64 now = -1;
        try {
            now = m_hooks.monotonicNanoseconds();
        } catch (...) {
            failSession(exceptionError(QStringLiteral("Recording clock")));
            return;
        }
        const QVector<qint64> frames = m_timeline.blackFramePts(now);
        for (qint64 pts : frames) {
            QString error;
            bool pushed = false;
            try {
                pushed = m_pipeline.pushBlackFrame &&
                         m_pipeline.pushBlackFrame(pts, &error);
            } catch (...) {
                error = exceptionError(QStringLiteral("Black-frame recording push"));
            }
            if (!pushed) {
                failSession(error.isEmpty()
                                ? QStringLiteral("Black-frame recording push failed")
                                : error);
                return;
            }
        }
    }

    bool processVideo(GstSample *sample, qint64 arrival)
    {
        RecordingVideoDescription description;
        if (!parseVideoDescription(sample, &description)) return false;
        const qint64 pts = samplePts(sample);
        if (arrival < 0) return false;

        if (!m_pipelineStarted) {
            if (!m_timeline.start(pts, description, arrival, m_negotiatedFrameRate)) {
                return false;
            }
            const RecordingVideoDescription locked = m_timeline.lockedDescription();
            const double fps = static_cast<double>(locked.fpsNumerator) /
                               static_cast<double>(locked.fpsDenominator);
            GstRecordingPipelineConfig config;
            config.videoSpoolPath = m_reservation.videoSpoolPath;
            config.audioSpoolPath = m_reservation.audioSpoolPath;
            config.temporaryMp4Path = m_reservation.temporaryMp4Path;
            config.video = locked;
            config.videoBitrateBitsPerSecond = recordingVideoBitrateBitsPerSecond(
                locked.width, locked.height, fps);
            config.preferredEncoder = m_preferredEncoder;
            QString error;
            bool started = false;
            try {
                started = m_pipeline.start && m_pipeline.start(config, sample, &error);
            } catch (...) {
                error = exceptionError(QStringLiteral("Recording pipeline Start"));
            }
            if (!started) {
                failSession(error.isEmpty() ? QStringLiteral("Recording pipeline Start failed")
                                            : error);
                return false;
            }
            m_pipelineStarted = true;
            (void)m_timeline.normalizeVideo(pts, arrival);
            return false;
        }

        if (m_timeline.dimensionsChanged(description)) {
            m_accepting->store(false, std::memory_order_release);
            m_warning = QStringLiteral("Recording saved because video dimensions changed; saved file: %1")
                            .arg(m_reservation.finalPath);
            notifyFinalizing();
            return true;
        }
        const std::optional<qint64> normalized = m_timeline.normalizeVideo(pts, arrival);
        if (!normalized.has_value()) return false;
        QString error;
        bool pushed = false;
        try {
            pushed = m_pipeline.pushVideo && m_pipeline.pushVideo(sample, *normalized, &error);
        } catch (...) {
            error = exceptionError(QStringLiteral("Video recording push"));
        }
        if (!pushed) failSession(error.isEmpty() ? QStringLiteral("Video recording push failed")
                                                 : error);
        return false;
    }

    void processAudio(GstSample *sample)
    {
        if (!m_pipelineStarted) return;
        const std::optional<qint64> normalized = m_timeline.normalizeAudio(samplePts(sample));
        if (!normalized.has_value()) return;
        QString error;
        bool pushed = false;
        try {
            pushed = m_pipeline.pushAudio && m_pipeline.pushAudio(sample, *normalized, &error);
        } catch (...) {
            error = exceptionError(QStringLiteral("Audio recording push"));
        }
        if (!pushed) failSession(error.isEmpty() ? QStringLiteral("Audio recording push failed")
                                                 : error);
    }

    void drainQueuedSnapshot()
    {
        qsizetype videosRemaining = m_videoQueue->size();
        qsizetype audiosRemaining = m_audioQueue->size();
        bool dimensionBoundary = false;
        while (m_active && !m_finalizing &&
               (videosRemaining > 0 || audiosRemaining > 0)) {
            if (videosRemaining > 0) {
                VideoEnvelope video = m_videoQueue->tryPopOwned();
                --videosRemaining;
                if (video.sample) {
                    dimensionBoundary = processVideo(video.sample,
                                                     video.arrivalNanoseconds);
                    gst_sample_unref(video.sample);
                }
                if (dimensionBoundary || !m_active || m_finalizing) break;
            }
            if (audiosRemaining > 0) {
                GstSample *audio = m_audioQueue->tryPopOwned();
                --audiosRemaining;
                if (audio) {
                    processAudio(audio);
                    gst_sample_unref(audio);
                }
            }
        }
        clearQueues();
    }

    void finishDrainScheduling()
    {
        m_drainScheduled->store(false, std::memory_order_release);
        if ((m_videoQueue->size() > 0 || m_audioQueue->size() > 0) &&
            m_active && !m_finalizing) {
            bool expected = false;
            if (m_drainScheduled->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                try {
                    if (m_hooks.drainInvocationScheduled) {
                        m_hooks.drainInvocationScheduled();
                    }
                } catch (...) {
                }
                QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
            }
        }
    }

    void notifyFinalizing()
    {
        if (!m_owner) return;
        QPointer<RecordingController> owner = m_owner;
        const quint64 generation = m_generation;
        const QString warning = m_warning;
        QMetaObject::invokeMethod(owner, [owner, generation, warning] {
            if (owner) owner->handleWorkerFinalizing(generation, warning);
        }, Qt::QueuedConnection);
    }

    void failSession(const QString &error)
    {
        const quint64 failedGeneration = m_generation;
        m_accepting->store(false, std::memory_order_release);
        if (!m_finalizing) {
            m_finalizing = true;
            notifyFinalizing();
        }
        cleanupPipeline();
        try {
            if (m_active) m_hooks.discard(m_reservation);
        } catch (...) {
        }
        resetSession();
        if (m_cancelled->load(std::memory_order_acquire)) return;
        if (m_owner) {
            QPointer<RecordingController> owner = m_owner;
            QMetaObject::invokeMethod(owner, [owner, failedGeneration, error] {
                if (owner) owner->handleWorkerFailed(failedGeneration, error);
            }, Qt::QueuedConnection);
        }
    }

    void cleanupPipeline()
    {
        if (m_pipeline.abort) {
            try { m_pipeline.abort(); } catch (...) {}
        }
        m_pipeline = {};
        m_pipelineStarted = false;
    }

    bool removeExplicitOwnedFinal()
    {
        if (m_ownedFinal.isEmpty()) return true;
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool removed = false;
            try {
                removed = m_hooks.removeOwnedFinal(m_ownedFinal);
            } catch (...) {
            }
            if (removed) {
                m_ownedFinal.clear();
                return true;
            }
        }
        return false;
    }

    void clearQueues()
    {
        QMutexLocker locker(m_queueGate);
        m_videoQueue->clear();
        m_audioQueue->clear();
    }

    void discardOwnedSession()
    {
        cleanupPipeline();
        if (m_active) {
            try { m_hooks.discard(m_reservation); } catch (...) {}
        }
        removeExplicitOwnedFinal();
        resetSession();
        clearQueues();
        m_drainScheduled->store(false, std::memory_order_release);
    }

    void resetSession()
    {
        if (m_blackTimer) m_blackTimer->stop();
        m_active = false;
        m_finalizing = false;
        m_pipelineStarted = false;
        m_timeline = RecordingTimeline{};
        m_pipeline = {};
        m_reservation = {};
        m_preferredEncoder.clear();
        m_warning.clear();
    }

    QPointer<RecordingController> m_owner;
    VideoEnvelopeQueue *m_videoQueue;
    GstSampleQueue *m_audioQueue;
    QMutex *m_queueGate;
    std::atomic_bool *m_drainScheduled;
    std::atomic_bool *m_accepting;
    std::atomic_bool *m_cancelled;
    RecordingControllerHooks m_hooks;
    RecordingPipelineSession m_pipeline;
    RecordingFileReservation m_reservation;
    RecordingTimeline m_timeline;
    QString m_preferredEncoder;
    QString m_warning;
    QString m_ownedFinal;
    int m_negotiatedFrameRate = 30;
    quint64 m_generation = 0;
    bool m_active = false;
    bool m_pipelineStarted = false;
    bool m_finalizing = false;
    QTimer *m_blackTimer = nullptr;
};

class RecordingControllerPrivate {
public:
    explicit RecordingControllerPrivate(int fps, RecordingControllerHooks suppliedHooks)
        : negotiatedFrameRate(fps),
          hooks(completedHooks(std::move(suppliedHooks))),
          videoQueue(RecordingController::VideoQueueCapacity),
          audioQueue(RecordingController::AudioQueueCapacity)
    {
    }

    void clearQueues()
    {
        QMutexLocker locker(&queueGate);
        videoQueue.clear();
        audioQueue.clear();
    }

    int negotiatedFrameRate;
    RecordingControllerHooks hooks;
    QMutex queueGate;
    VideoEnvelopeQueue videoQueue;
    GstSampleQueue audioQueue;
    QThread workerThread;
    RecordingWorker *worker = nullptr;
    std::atomic_bool available{false};
    std::atomic<RecordingState> state{RecordingState::Idle};
    std::atomic_bool accepting{false};
    std::atomic_bool cancelled{false};
    std::atomic_bool drainScheduled{false};
    quint64 generation = 0;
    QSet<QString> cleanedDirectories;
};

RecordingController::RecordingController(int negotiatedFrameRate,
                                         RecordingControllerHooks hooks,
                                         QObject *parent)
    : QObject(parent),
      d(std::make_unique<RecordingControllerPrivate>(negotiatedFrameRate,
                                                     std::move(hooks)))
{
    qRegisterMetaType<RecordingState>();
    qRegisterMetaType<RecordingResult>();
    d->worker = new RecordingWorker(this, &d->videoQueue, &d->audioQueue,
                                    &d->queueGate,
                                    &d->drainScheduled, &d->accepting,
                                    &d->cancelled, d->hooks);
    d->worker->moveToThread(&d->workerThread);
    d->workerThread.start();
}

RecordingController::~RecordingController()
{
    discard();
    if (d->workerThread.isRunning() && d->worker) {
        QMetaObject::invokeMethod(d->worker, [worker = d->worker] {
            worker->shutdown();
            delete worker;
        }, Qt::BlockingQueuedConnection);
        d->worker = nullptr;
        d->workerThread.quit();
        d->workerThread.wait();
    }
}

bool RecordingController::available() const
{
    return d->available.load(std::memory_order_acquire);
}

RecordingState RecordingController::state() const
{
    return d->state.load(std::memory_order_acquire);
}

RecordingStartResult RecordingController::start(const RecordingOptions &options)
{
    if (state() != RecordingState::Idle) {
        return {false, QStringLiteral("A recording is already active or being finalized")};
    }
    if (!available()) {
        return {false, QStringLiteral("No recordable mirrored video is available")};
    }
    if (options.format != RecordingFormat::Mp4) {
        return {false, QStringLiteral("Unsupported recording format")};
    }
    if (options.outputDirectory.trimmed().isEmpty()) {
        return {false, QStringLiteral("Recording output directory is empty")};
    }

    GstRecordingCapabilityResult capability;
    QMetaObject::invokeMethod(d->worker, [worker = d->worker, &capability] {
        capability = worker->probeCapabilities();
    }, Qt::BlockingQueuedConnection);
    if (!capability.available) {
        return {false, capability.error.isEmpty()
                           ? QStringLiteral("Recording capability is unavailable")
                           : capability.error};
    }

    QString directoryError;
    try {
        directoryError = d->hooks.ensureOutputDirectory(options.outputDirectory);
    } catch (...) {
        directoryError = exceptionError(QStringLiteral("Recording directory validation"));
    }
    if (!directoryError.isEmpty()) return {false, directoryError};

    const QString absoluteDirectory = QDir(options.outputDirectory).absolutePath();
    const QString directoryKey = QDir::cleanPath(absoluteDirectory).toLower();
    if (!d->cleanedDirectories.contains(directoryKey)) {
        try {
            (void)d->hooks.cleanupStaleTemporaryFiles(absoluteDirectory);
        } catch (...) {
        }
        d->cleanedDirectories.insert(directoryKey);
    }

    RecordingFileReservationResult reservationResult;
    try {
        reservationResult = d->hooks.reserve(
            absoluteDirectory, d->hooks.now(), d->hooks.uuid());
    } catch (...) {
        return {false, exceptionError(QStringLiteral("Recording file reservation"))};
    }
    if (!reservationResult.reservation.has_value()) {
        return {false, reservationResult.error.isEmpty()
                           ? QStringLiteral("Recording file reservation failed")
                           : reservationResult.error};
    }

    d->accepting.store(false, std::memory_order_release);
    d->cancelled.store(false, std::memory_order_release);
    d->clearQueues();
    d->drainScheduled.store(false, std::memory_order_release);
    const quint64 generation = ++d->generation;
    QString workerBeginError;
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, reservation = *reservationResult.reservation,
         preferred = capability.preferredEncoder,
         fps = d->negotiatedFrameRate, generation, &workerBeginError] {
            workerBeginError = worker->begin(reservation, preferred, fps, generation);
        }, Qt::BlockingQueuedConnection);
    if (!workerBeginError.isEmpty()) return {false, workerBeginError};
    if (d->cancelled.load(std::memory_order_acquire)) {
        return {false, QStringLiteral("Recording Start was cancelled")};
    }
    d->state.store(RecordingState::Recording, std::memory_order_release);
    d->accepting.store(true, std::memory_order_release);
    emit stateChanged(RecordingState::Recording);
    return {true, {}};
}

void RecordingController::stop()
{
    RecordingState expected = RecordingState::Recording;
    if (!d->state.compare_exchange_strong(
            expected, RecordingState::Finalizing, std::memory_order_acq_rel)) {
        return;
    }
    d->accepting.store(false, std::memory_order_release);
    emit stateChanged(RecordingState::Finalizing);
    const quint64 generation = d->generation;
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, generation] { worker->finalize(generation); },
        Qt::QueuedConnection);
}

void RecordingController::discard()
{
    const RecordingState prior = d->state.exchange(
        RecordingState::Idle, std::memory_order_acq_rel);
    d->accepting.store(false, std::memory_order_release);
    const quint64 generation = d->generation;
    if (prior != RecordingState::Idle) {
        d->cancelled.store(true, std::memory_order_release);
        ++d->generation;
    }
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, generation] { worker->discardSynchronously(generation); },
        Qt::BlockingQueuedConnection);
    d->clearQueues();
    d->drainScheduled.store(false, std::memory_order_release);
    if (prior != RecordingState::Idle) emit stateChanged(RecordingState::Idle);
}

bool RecordingController::tryEnqueueVideoSample(GstSample *borrowedSample) noexcept
{
    RecordingVideoDescription description;
    if (!parseVideoDescription(borrowedSample, &description)) return false;
    setAvailabilityFromAnyThread(true);
    if (!d->accepting.load(std::memory_order_acquire) ||
        state() != RecordingState::Recording) {
        return false;
    }
    qint64 arrival = -1;
    try {
        arrival = d->hooks.monotonicNanoseconds();
    } catch (...) {
        return false;
    }
    if (arrival < 0) return false;
    {
        std::unique_lock<QMutex> locker(d->queueGate, std::try_to_lock);
        if (!locker.owns_lock()) return false;
        if (!d->accepting.load(std::memory_order_acquire) ||
            state() != RecordingState::Recording ||
            !d->videoQueue.tryPushBorrowed(borrowedSample, arrival)) {
            return false;
        }
    }
    bool expected = false;
    if (d->drainScheduled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        try {
            if (d->hooks.drainInvocationScheduled) d->hooks.drainInvocationScheduled();
        } catch (...) {
        }
        QMetaObject::invokeMethod(d->worker, [worker = d->worker] { worker->drain(); },
                                  Qt::QueuedConnection);
    }
    return true;
}

bool RecordingController::tryEnqueueAudioSample(GstSample *borrowedSample) noexcept
{
    if (!isAudioSample(borrowedSample) ||
        !d->accepting.load(std::memory_order_acquire) ||
        state() != RecordingState::Recording) {
        return false;
    }
    {
        std::unique_lock<QMutex> locker(d->queueGate, std::try_to_lock);
        if (!locker.owns_lock()) return false;
        if (!d->accepting.load(std::memory_order_acquire) ||
            state() != RecordingState::Recording ||
            !d->audioQueue.tryPushBorrowed(borrowedSample)) {
            return false;
        }
    }
    bool expected = false;
    if (d->drainScheduled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        try {
            if (d->hooks.drainInvocationScheduled) d->hooks.drainInvocationScheduled();
        } catch (...) {
        }
        QMetaObject::invokeMethod(d->worker, [worker = d->worker] { worker->drain(); },
                                  Qt::QueuedConnection);
    }
    return true;
}

void RecordingController::sessionEnded(bool canFinalize)
{
    setAvailabilityFromAnyThread(false);
    if (state() != RecordingState::Recording) return;
    if (canFinalize) {
        stop();
        return;
    }
    d->accepting.store(false, std::memory_order_release);
    d->state.store(RecordingState::Finalizing, std::memory_order_release);
    emit stateChanged(RecordingState::Finalizing);
    const quint64 generation = d->generation;
    QMetaObject::invokeMethod(d->worker, [worker = d->worker, generation] {
        worker->failUnfinalizable(
            generation,
            QStringLiteral("Mirrored session ended before recording could be finalized"));
    }, Qt::QueuedConnection);
}

void RecordingController::setAvailabilityFromAnyThread(bool newAvailability)
{
    const bool old = d->available.exchange(newAvailability, std::memory_order_acq_rel);
    if (old == newAvailability) return;
    if (QThread::currentThread() == thread()) {
        emit availabilityChanged(newAvailability);
        return;
    }
    QPointer<RecordingController> self(this);
    QMetaObject::invokeMethod(this, [self, newAvailability] {
        if (self && self->available() == newAvailability) {
            emit self->availabilityChanged(newAvailability);
        }
    }, Qt::QueuedConnection);
}

void RecordingController::handleWorkerFinalizing(quint64 generation,
                                                  const QString &)
{
    if (generation != d->generation || state() != RecordingState::Recording) return;
    d->accepting.store(false, std::memory_order_release);
    d->state.store(RecordingState::Finalizing, std::memory_order_release);
    emit stateChanged(RecordingState::Finalizing);
}

void RecordingController::handleWorkerFinished(quint64 generation,
                                                const RecordingResult &result)
{
    if (generation != d->generation || d->cancelled.load(std::memory_order_acquire)) return;
    QMetaObject::invokeMethod(d->worker,
        [worker = d->worker, generation, path = result.finalPath] {
            worker->relinquishCommittedFinal(generation, path);
        }, Qt::QueuedConnection);
    d->accepting.store(false, std::memory_order_release);
    d->state.store(RecordingState::Idle, std::memory_order_release);
    emit stateChanged(RecordingState::Idle);
    emit finished(result);
}

void RecordingController::handleWorkerFailed(quint64 generation, const QString &error)
{
    if (generation != d->generation || d->cancelled.load(std::memory_order_acquire)) return;
    d->accepting.store(false, std::memory_order_release);
    d->state.store(RecordingState::Idle, std::memory_order_release);
    emit stateChanged(RecordingState::Idle);
    emit failed(error);
}

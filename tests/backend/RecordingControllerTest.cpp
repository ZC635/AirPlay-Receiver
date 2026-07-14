#include "backend/RecordingController.h"

#include <QtTest>

#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QElapsedTimer>

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <QUrl>

#include <atomic>
#include <functional>
#include <thread>
#include <stdexcept>

namespace {

GstSample *makeVideoSample(int width = 640, int height = 360,
                           GstClockTime pts = 5 * GST_SECOND,
                           const char *format = "RGBA")
{
    GstBuffer *buffer = gst_buffer_new_allocate(
        nullptr, static_cast<gsize>(width * height * 4), nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, format,
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, 30, 1,
        "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

GstSample *makeAudioSample(GstClockTime pts = 5 * GST_SECOND)
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 441 * 2 * sizeof(gint16), nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GstCaps *caps = gst_caps_new_simple(
        "audio/x-raw", "format", G_TYPE_STRING, "S16LE",
        "rate", G_TYPE_INT, 44100, "channels", G_TYPE_INT, 2,
        "layout", G_TYPE_STRING, "interleaved", nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

GstSample *makeUndersizedVideoSample()
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 1, nullptr);
    GST_BUFFER_PTS(buffer) = 0;
    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "RGBA",
        "width", G_TYPE_INT, 640, "height", G_TYPE_INT, 360,
        "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

struct MediaCounts {
    int video = 0;
    int audio = 0;
};

bool discoverCounts(const QString &path, MediaCounts *counts, QString *error)
{
    GError *createError = nullptr;
    GstDiscoverer *discoverer = gst_discoverer_new(10 * GST_SECOND, &createError);
    if (!discoverer) {
        *error = QString::fromUtf8(createError ? createError->message
                                               : "discoverer creation failed");
        g_clear_error(&createError);
        return false;
    }
    GError *discoverError = nullptr;
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    GstDiscovererInfo *info = gst_discoverer_discover_uri(
        discoverer, uri.constData(), &discoverError);
    if (!info || discoverError) {
        *error = QString::fromUtf8(discoverError ? discoverError->message
                                                 : "discovery failed");
        g_clear_error(&discoverError);
        if (info) gst_discoverer_info_unref(info);
        g_object_unref(discoverer);
        return false;
    }
    GList *streams = gst_discoverer_info_get_stream_list(info);
    for (GList *entry = streams; entry; entry = entry->next) {
        auto *stream = GST_DISCOVERER_STREAM_INFO(entry->data);
        if (GST_IS_DISCOVERER_VIDEO_INFO(stream)) ++counts->video;
        if (GST_IS_DISCOVERER_AUDIO_INFO(stream)) ++counts->audio;
    }
    gst_discoverer_stream_info_list_free(streams);
    gst_discoverer_info_unref(info);
    g_object_unref(discoverer);
    return true;
}

struct FakeEnvironment {
    int probeCalls = 0;
    int ensureCalls = 0;
    int cleanupCalls = 0;
    int reserveCalls = 0;
    std::atomic_int discardCalls{0};
    int commitCalls = 0;
    int pipelineStartCalls = 0;
    int finalizeCalls = 0;
    int abortCalls = 0;
    QString directoryError;
    QString commitError;
    QStringList reservedDirectories;
    QStringList removedOwnedFinalPaths;
    QStringList successfulOwnedFinalRemovals;
    QVector<GstRecordingPipelineConfig> startedConfigs;
    QVector<qint64> videoPts;
    QVector<qint64> audioPts;
    QVector<qint64> blackPts;
    bool finalizeSucceeds = true;
    QString finalizeError;
    bool waitForCancellationInFinalize = false;
    std::atomic_bool blockPipelineStart{false};
    std::atomic_bool pipelineStartEntered{false};
    std::atomic_bool releasePipelineStart{false};
    std::atomic_bool blockVideoPush{false};
    std::atomic_bool videoPushEntered{false};
    std::atomic_bool releaseVideoPush{false};
    std::atomic_int videoPushCount{0};
    std::atomic_int videoPushCountAtFinalize{-1};
    std::atomic_int videoPushCountAtWorkerFinalizeEntry{-1};
    std::atomic_int videoPushCountAtAbort{-1};
    std::atomic_int removeOwnedFinalFailuresRemaining{0};
    std::atomic_int removeOwnedFinalThrowsRemaining{0};
    std::function<void()> afterVideoPush;
    std::atomic_int scheduledDrainCount{0};
    std::atomic_int completedDrainCount{0};
    std::atomic<qint64> monotonicNow{0};
    std::atomic_bool blockMonotonicClock{false};
    std::atomic_bool monotonicClockEntered{false};
    std::atomic_bool releaseMonotonicClock{false};
    std::atomic_bool throwFromMonotonicClock{false};
    std::atomic_int observedFinalizeDeadline{0};
    std::atomic_bool blockCommit{false};
    std::atomic_bool commitEntered{false};
    std::atomic_bool releaseCommit{false};
    bool throwFromPipelineFactory = false;
    std::atomic<QThread *> capabilityProbeThread{nullptr};
    std::atomic<QThread *> pipelineFactoryThread{nullptr};
    mutable QMutex mutex;
    GstRecordingCapabilityResult capability{true, QStringLiteral("openh264enc"), {}};
    RecordingFileReservationResult reservation{
        RecordingFileReservation{QStringLiteral("video.part"), QStringLiteral("audio.part"),
                                 QStringLiteral("mp4.part"), QStringLiteral("final.mp4")}, {}};

    RecordingControllerHooks hooks()
    {
        RecordingControllerHooks hooks;
        hooks.probeCapabilities = [this] {
            capabilityProbeThread.store(QThread::currentThread(), std::memory_order_release);
            ++probeCalls;
            return capability;
        };
        hooks.ensureOutputDirectory = [this](const QString &) {
            ++ensureCalls;
            return directoryError;
        };
        hooks.cleanupStaleTemporaryFiles = [this](const QString &) {
            ++cleanupCalls;
            return QStringList{};
        };
        hooks.discard = [this](const RecordingFileReservation &) {
            discardCalls.fetch_add(1, std::memory_order_relaxed);
        };
        hooks.reserve = [this](const QString &directory, const QDateTime &, const QUuid &) {
            QMutexLocker locker(&mutex);
            ++reserveCalls;
            reservedDirectories.append(QDir::cleanPath(directory));
            return reservation;
        };
        hooks.commit = [this](const RecordingFileReservation &) {
            {
                QMutexLocker locker(&mutex);
                ++commitCalls;
            }
            commitEntered.store(true, std::memory_order_release);
            while (blockCommit.load(std::memory_order_acquire) &&
                   !releaseCommit.load(std::memory_order_acquire)) {
                QThread::msleep(1);
            }
            return commitError;
        };
        hooks.now = [] { return QDateTime(QDate(2026, 7, 14), QTime(10, 0)); };
        hooks.uuid = [] { return QUuid(QStringLiteral("{11111111-2222-3333-4444-555555555555}")); };
        hooks.monotonicNanoseconds = [this] {
            if (throwFromMonotonicClock.load(std::memory_order_acquire)) {
                throw std::runtime_error("injected recording clock failure");
            }
            monotonicClockEntered.store(true, std::memory_order_release);
            while (blockMonotonicClock.load(std::memory_order_acquire) &&
                   !releaseMonotonicClock.load(std::memory_order_acquire)) {
                QThread::yieldCurrentThread();
            }
            return monotonicNow.load(std::memory_order_acquire);
        };
        hooks.drainInvocationScheduled = [this] {
            scheduledDrainCount.fetch_add(1, std::memory_order_relaxed);
        };
        hooks.workerDrainCompleted = [this] {
            completedDrainCount.fetch_add(1, std::memory_order_release);
        };
        hooks.workerFinalizeEntered = [this] {
            videoPushCountAtWorkerFinalizeEntry.store(
                videoPushCount.load(std::memory_order_acquire),
                std::memory_order_release);
        };
        hooks.createPipeline = [this] {
            pipelineFactoryThread.store(QThread::currentThread(), std::memory_order_release);
            if (throwFromPipelineFactory) {
                throw std::runtime_error("injected pipeline factory failure");
            }
            RecordingPipelineSession session;
            session.start = [this](const GstRecordingPipelineConfig &config,
                                   GstSample *, QString *) {
                QMutexLocker locker(&mutex);
                ++pipelineStartCalls;
                startedConfigs.append(config);
                pipelineStartEntered.store(true, std::memory_order_release);
                locker.unlock();
                while (blockPipelineStart.load(std::memory_order_acquire) &&
                       !releasePipelineStart.load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                return true;
            };
            session.pushVideo = [this](GstSample *, qint64 pts, QString *) {
                {
                    QMutexLocker locker(&mutex);
                    videoPts.append(pts);
                }
                videoPushCount.fetch_add(1, std::memory_order_acq_rel);
                videoPushEntered.store(true, std::memory_order_release);
                while (blockVideoPush.load(std::memory_order_acquire) &&
                       !releaseVideoPush.load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                if (afterVideoPush) afterVideoPush();
                return true;
            };
            session.pushAudio = [this](GstSample *, qint64 pts, QString *) {
                QMutexLocker locker(&mutex);
                audioPts.append(pts);
                return true;
            };
            session.pushBlackFrame = [this](qint64 pts, QString *) {
                QMutexLocker locker(&mutex);
                blackPts.append(pts);
                return true;
            };
            session.finalize = [this](int deadline, const std::atomic_bool &cancelled) {
                observedFinalizeDeadline.store(deadline, std::memory_order_release);
                videoPushCountAtFinalize.store(
                    videoPushCount.load(std::memory_order_acquire),
                    std::memory_order_release);
                {
                    QMutexLocker locker(&mutex);
                    ++finalizeCalls;
                }
                while (waitForCancellationInFinalize &&
                       !cancelled.load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                return GstRecordingFinalizeResult{finalizeSucceeds, finalizeError};
            };
            session.abort = [this] {
                QMutexLocker locker(&mutex);
                ++abortCalls;
                videoPushCountAtAbort.store(
                    videoPushCount.load(std::memory_order_acquire),
                    std::memory_order_release);
            };
            return session;
        };
        hooks.removeOwnedFinal = [this](const QString &path) {
            QMutexLocker locker(&mutex);
            removedOwnedFinalPaths.append(path);
            int throwsRemaining = removeOwnedFinalThrowsRemaining.load(
                std::memory_order_acquire);
            if (throwsRemaining > 0) {
                removeOwnedFinalThrowsRemaining.fetch_sub(1, std::memory_order_acq_rel);
                throw std::runtime_error("injected owned final removal failure");
            }
            int failuresRemaining = removeOwnedFinalFailuresRemaining.load(
                std::memory_order_acquire);
            if (failuresRemaining > 0) {
                removeOwnedFinalFailuresRemaining.fetch_sub(1, std::memory_order_acq_rel);
                return false;
            }
            successfulOwnedFinalRemovals.append(path);
            return true;
        };
        return hooks;
    }

    int startCount() const { QMutexLocker locker(&mutex); return pipelineStartCalls; }
    int finalizeCount() const { QMutexLocker locker(&mutex); return finalizeCalls; }
    int commitCount() const { QMutexLocker locker(&mutex); return commitCalls; }
    QVector<GstRecordingPipelineConfig> configs() const {
        QMutexLocker locker(&mutex); return startedConfigs;
    }
    QVector<qint64> pushedVideoPts() const { QMutexLocker locker(&mutex); return videoPts; }
    QVector<qint64> pushedAudioPts() const { QMutexLocker locker(&mutex); return audioPts; }
    QVector<qint64> pushedBlackPts() const { QMutexLocker locker(&mutex); return blackPts; }
    qint64 lastVideoPts() const {
        QMutexLocker locker(&mutex); return videoPts.isEmpty() ? -1 : videoPts.constLast();
    }
    QStringList directories() const { QMutexLocker locker(&mutex); return reservedDirectories; }
    QStringList removedFinals() const { QMutexLocker locker(&mutex); return removedOwnedFinalPaths; }
    QStringList successfulRemovals() const {
        QMutexLocker locker(&mutex); return successfulOwnedFinalRemovals;
    }
};

void observeVideo(RecordingController &controller, GstSample *sample = nullptr)
{
    GstSample *owned = sample ? sample : makeVideoSample();
    QVERIFY(!controller.tryEnqueueVideoSample(owned));
    gst_sample_unref(owned);
    QCoreApplication::processEvents();
}

} // namespace

class RecordingControllerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        gst_init(nullptr, nullptr);
    }

    void availabilityRequiresValidRgbaVideoAndIgnoresAudio()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        QSignalSpy availabilitySpy(&controller, &RecordingController::availabilityChanged);
        QVERIFY(!controller.available());

        GstSample *audio = makeAudioSample();
        QVERIFY(!controller.tryEnqueueAudioSample(audio));
        gst_sample_unref(audio);
        QVERIFY(!controller.available());

        GstSample *wrongFormat = makeVideoSample(640, 360, 0, "BGRA");
        QVERIFY(!controller.tryEnqueueVideoSample(wrongFormat));
        gst_sample_unref(wrongFormat);
        QVERIFY(!controller.available());

        GstSample *undersized = makeUndersizedVideoSample();
        QVERIFY(!controller.tryEnqueueVideoSample(undersized));
        gst_sample_unref(undersized);
        QVERIFY(!controller.available());

        observeVideo(controller);
        QVERIFY(controller.available());
        QCOMPARE(availabilitySpy.count(), 1);
        QCOMPARE(availabilitySpy.constFirst().constFirst().toBool(), true);
    }

    void staleQueuedAvailabilitySignalCannotOverrideSessionEnd()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        QSignalSpy availabilitySpy(&controller, &RecordingController::availabilityChanged);
        GstSample *video = makeVideoSample();
        std::thread producer([&controller, video] {
            (void)controller.tryEnqueueVideoSample(video);
        });
        producer.join();
        QVERIFY(controller.available());

        controller.sessionEnded(true);
        QVERIFY(!controller.available());
        QCoreApplication::processEvents();

        QCOMPARE(availabilitySpy.count(), 1);
        QCOMPARE(availabilitySpy.constFirst().constFirst().toBool(), false);
        gst_sample_unref(video);
    }

    void startValidationIsSynchronousSpecificAndNonPolluting()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        const RecordingOptions options{QStringLiteral("C:/recordings"), RecordingFormat::Mp4};

        RecordingStartResult result = controller.start(options);
        QVERIFY(!result.accepted);
        QVERIFY(result.error.contains(QStringLiteral("video"), Qt::CaseInsensitive));
        QCOMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(environment.probeCalls, 0);

        observeVideo(controller);
        environment.capability = {false, {}, QStringLiteral("missing H.264 encoder")};
        result = controller.start(options);
        QVERIFY(!result.accepted);
        QCOMPARE(result.error, QStringLiteral("missing H.264 encoder"));
        QCOMPARE(environment.ensureCalls, 0);
        QCOMPARE(environment.reserveCalls, 0);
        QCOMPARE(controller.state(), RecordingState::Idle);

        environment.capability = {true, QStringLiteral("openh264enc"), {}};
        environment.directoryError = QStringLiteral("directory is read-only");
        result = controller.start(options);
        QVERIFY(!result.accepted);
        QCOMPARE(result.error, environment.directoryError);
        QCOMPARE(environment.reserveCalls, 0);
        QCOMPARE(controller.state(), RecordingState::Idle);

        environment.directoryError.clear();
        environment.reservation = {{}, QStringLiteral("temporary reservation failed")};
        result = controller.start(options);
        QVERIFY(!result.accepted);
        QCOMPARE(result.error, QStringLiteral("temporary reservation failed"));
        QCOMPARE(controller.state(), RecordingState::Idle);
    }

    void acceptedStartRejectsDuplicateWithoutAnotherReservation()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);

        const RecordingStartResult first = controller.start(
            {QStringLiteral("C:/recordings"), RecordingFormat::Mp4});
        QVERIFY2(first.accepted, qPrintable(first.error));
        QCOMPARE(controller.state(), RecordingState::Recording);
        QCOMPARE(environment.reserveCalls, 1);

        const RecordingStartResult duplicate = controller.start(
            {QStringLiteral("C:/other"), RecordingFormat::Mp4});
        QVERIFY(!duplicate.accepted);
        QVERIFY(duplicate.error.contains(QStringLiteral("already"), Qt::CaseInsensitive));
        QCOMPARE(environment.reserveCalls, 1);
        QCOMPARE(controller.state(), RecordingState::Recording);

        controller.discard();
    }

    void pipelineFactoryFailureRejectsStartSynchronouslyAndDiscardsReservation()
    {
        FakeEnvironment environment;
        environment.throwFromPipelineFactory = true;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);

        const RecordingStartResult result = controller.start(
            {QStringLiteral("C:/recordings"), RecordingFormat::Mp4});

        QVERIFY(!result.accepted);
        QVERIFY(result.error.contains(QStringLiteral("factory"), Qt::CaseInsensitive));
        QCOMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(environment.discardCalls.load(std::memory_order_acquire), 1);
    }

    void capabilityProbeAndPipelineObjectsLiveOnlyOnWorkerThread()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        QVERIFY(environment.capabilityProbeThread.load(std::memory_order_acquire));
        QVERIFY(environment.pipelineFactoryThread.load(std::memory_order_acquire));
        QVERIFY(environment.capabilityProbeThread.load(std::memory_order_acquire) !=
                controller.thread());
        QCOMPARE(environment.capabilityProbeThread.load(std::memory_order_acquire),
                 environment.pipelineFactoryThread.load(std::memory_order_acquire));
        controller.discard();
    }

    void recordingOptionsAndReservationAreImmutablePerStart()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        RecordingOptions options{QStringLiteral("C:/first-recordings"), RecordingFormat::Mp4};

        QVERIFY(controller.start(options).accepted);
        options.outputDirectory = QStringLiteral("C:/second-recordings");
        GstSample *firstVideo = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(firstVideo));
        gst_sample_unref(firstVideo);
        QTRY_COMPARE(environment.startCount(), 1);
        QCOMPARE(environment.directories(),
                 QStringList{QDir(QStringLiteral("C:/first-recordings")).absolutePath()});
        QCOMPARE(environment.configs().constFirst().videoSpoolPath,
                 environment.reservation.reservation->videoSpoolPath);

        controller.discard();
        QVERIFY(controller.start(options).accepted);
        QCOMPARE(environment.directories().constLast(),
                 QDir(QStringLiteral("C:/second-recordings")).absolutePath());
        controller.discard();
    }

    void stateMachineFinishesAndFailsInExactOrder()
    {
        FakeEnvironment successEnvironment;
        RecordingController successController(30, successEnvironment.hooks());
        observeVideo(successController);
        QSignalSpy stateSpy(&successController, &RecordingController::stateChanged);
        QSignalSpy finishedSpy(&successController, &RecordingController::finished);
        QSignalSpy failedSpy(&successController, &RecordingController::failed);

        QVERIFY(successController.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(successController.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(successEnvironment.startCount(), 1);
        successController.stop();
        successController.stop();
        QVERIFY(!successController.start({QStringLiteral("C:/other"), RecordingFormat::Mp4}).accepted);
        QTRY_COMPARE(successController.state(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(successEnvironment.finalizeCount(), 1);
        QCOMPARE(successEnvironment.commitCount(), 1);
        QCOMPARE(stateSpy.count(), 3);
        QCOMPARE(qvariant_cast<RecordingState>(stateSpy.at(0).at(0)), RecordingState::Recording);
        QCOMPARE(qvariant_cast<RecordingState>(stateSpy.at(1).at(0)), RecordingState::Finalizing);
        QCOMPARE(qvariant_cast<RecordingState>(stateSpy.at(2).at(0)), RecordingState::Idle);

        FakeEnvironment failureEnvironment;
        failureEnvironment.finalizeSucceeds = false;
        failureEnvironment.finalizeError = QStringLiteral("injected EOS failure");
        RecordingController failureController(30, failureEnvironment.hooks());
        observeVideo(failureController);
        QSignalSpy failureFinishedSpy(&failureController, &RecordingController::finished);
        QSignalSpy failureFailedSpy(&failureController, &RecordingController::failed);
        QVERIFY(failureController.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        video = makeVideoSample();
        QVERIFY(failureController.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(failureEnvironment.startCount(), 1);
        failureController.stop();
        QTRY_COMPARE(failureController.state(), RecordingState::Idle);
        QCOMPARE(failureFinishedSpy.count(), 0);
        QCOMPARE(failureFailedSpy.count(), 1);
        QVERIFY(failureFailedSpy.constFirst().constFirst().toString().contains(QStringLiteral("EOS")));
    }

    void discardIsSynchronousAndEmitsNoCompletion()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QSignalSpy failedSpy(&controller, &RecordingController::failed);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        controller.discard();
        QCOMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(environment.discardCalls.load(std::memory_order_acquire), 1);
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);

        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        environment.waitForCancellationInFinalize = true;
        controller.stop();
        QTRY_COMPARE(environment.finalizeCount(), 1);
        controller.discard();
        QCOMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
    }

    void unfinalizableSessionFailureCompletesOnlyAfterWorkerCleanup()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        QSignalSpy failedSpy(&controller, &RecordingController::failed);

        controller.sessionEnded(false);

        QCOMPARE(controller.state(), RecordingState::Finalizing);
        QCOMPARE(failedSpy.count(), 0);
        QTRY_COMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(environment.discardCalls.load(std::memory_order_acquire), 1);
        QCOMPARE(failedSpy.count(), 1);
    }

    void firstPostStartVideoDefinesDimensionsAndCommonTimeOrigin()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        GstSample *observed = makeVideoSample(320, 180, 2 * GST_SECOND);
        observeVideo(controller, observed);
        QCOMPARE(environment.startCount(), 0);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        GstSample *earlyAudio = makeAudioSample(9 * GST_SECOND);
        QVERIFY(controller.tryEnqueueAudioSample(earlyAudio));
        gst_sample_unref(earlyAudio);
        GstSample *first = makeVideoSample(1280, 720, 10 * GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        GstSample *second = makeVideoSample(1280, 720, 10'500'000'000);
        QVERIFY(controller.tryEnqueueVideoSample(second));
        gst_sample_unref(second);
        GstSample *lateAudio = makeAudioSample(10'700'000'000);
        QVERIFY(controller.tryEnqueueAudioSample(lateAudio));
        gst_sample_unref(lateAudio);

        QTRY_COMPARE(environment.startCount(), 1);
        QTRY_COMPARE(environment.pushedVideoPts().size(), 1);
        QTRY_COMPARE(environment.pushedAudioPts().size(), 1);
        const GstRecordingPipelineConfig config = environment.configs().constFirst();
        QCOMPARE(config.video.width, 1280);
        QCOMPARE(config.video.height, 720);
        QCOMPARE(environment.pushedVideoPts(), QVector<qint64>{500'000'000});
        QCOMPARE(environment.pushedAudioPts(), QVector<qint64>{700'000'000});
        controller.discard();
    }

    void audioQueuedBeforeFirstVideoWaitsForTimelineOrigin()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        GstSample *early = makeAudioSample(500'000'000);
        QVERIFY(controller.tryEnqueueAudioSample(early));
        gst_sample_unref(early);
        GstSample *late = makeAudioSample(2 * GST_SECOND);
        QVERIFY(controller.tryEnqueueAudioSample(late));
        gst_sample_unref(late);

        QTRY_COMPARE(environment.completedDrainCount.load(std::memory_order_acquire), 1);
        QCOMPARE(environment.pushedAudioPts(), QVector<qint64>{});
        const int quietScheduleCount = environment.scheduledDrainCount.load(
            std::memory_order_acquire);
        QCoreApplication::processEvents();
        QCOMPARE(environment.scheduledDrainCount.load(std::memory_order_acquire),
                 quietScheduleCount);

        GstSample *firstVideo = makeVideoSample(640, 360, GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(firstVideo));
        gst_sample_unref(firstVideo);
        QTRY_COMPARE(environment.startCount(), 1);
        QTRY_COMPARE(environment.pushedAudioPts(), QVector<qint64>{GST_SECOND});
        controller.discard();
    }

    void boundedQueuesDropImmediatelyAndCoalesceDrainInvocation()
    {
        static_assert(RecordingController::VideoQueueCapacity > 0);
        static_assert(RecordingController::AudioQueueCapacity == 64);
        FakeEnvironment environment;
        environment.blockPipelineStart.store(true, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        GstSample *first = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_VERIFY(environment.pipelineStartEntered.load(std::memory_order_acquire));
        QCOMPARE(environment.scheduledDrainCount.load(std::memory_order_acquire), 1);

        for (qsizetype index = 0; index < RecordingController::VideoQueueCapacity; ++index) {
            GstSample *sample = makeVideoSample(640, 360, 6 * GST_SECOND + index);
            QVERIFY(controller.tryEnqueueVideoSample(sample));
            gst_sample_unref(sample);
        }
        GstSample *videoOverflow = makeVideoSample();
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!controller.tryEnqueueVideoSample(videoOverflow));
        QVERIFY(timer.elapsed() < 100);
        gst_sample_unref(videoOverflow);

        for (qsizetype index = 0; index < RecordingController::AudioQueueCapacity; ++index) {
            GstSample *sample = makeAudioSample(7 * GST_SECOND + index);
            QVERIFY(controller.tryEnqueueAudioSample(sample));
            gst_sample_unref(sample);
        }
        GstSample *audioOverflow = makeAudioSample();
        timer.restart();
        QVERIFY(!controller.tryEnqueueAudioSample(audioOverflow));
        QVERIFY(timer.elapsed() < 100);
        gst_sample_unref(audioOverflow);
        QCOMPARE(environment.scheduledDrainCount.load(std::memory_order_acquire), 1);

        environment.releasePipelineStart.store(true, std::memory_order_release);
        QTRY_VERIFY(environment.pushedVideoPts().size() > 0);
        QTRY_VERIFY(environment.scheduledDrainCount.load(std::memory_order_acquire) > 1);
        controller.discard();
    }

    void drainAlternatesAudioWhileVideoProducerKeepsQueueNonEmpty()
    {
        FakeEnvironment environment;
        environment.blockPipelineStart.store(true, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_VERIFY(environment.pipelineStartEntered.load(std::memory_order_acquire));

        std::atomic_bool checkpointReached{false};
        std::atomic_bool releaseCheckpoint{false};
        environment.afterVideoPush = [&] {
            const int count = environment.videoPushCount.load(std::memory_order_acquire);
            if (count < 32) {
                GstSample *replacement = makeVideoSample(
                    640, 360, static_cast<GstClockTime>(count + 2) * GST_SECOND);
                (void)controller.tryEnqueueVideoSample(replacement);
                gst_sample_unref(replacement);
            }
            if (count == 4) {
                checkpointReached.store(true, std::memory_order_release);
                while (!releaseCheckpoint.load(std::memory_order_acquire)) {
                    QThread::yieldCurrentThread();
                }
            }
        };

        GstSample *audio = makeAudioSample(GST_SECOND);
        QVERIFY(controller.tryEnqueueAudioSample(audio));
        gst_sample_unref(audio);
        GstSample *video = makeVideoSample(640, 360, GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        environment.releasePipelineStart.store(true, std::memory_order_release);

        QTRY_VERIFY(checkpointReached.load(std::memory_order_acquire));
        const qsizetype audioCountAtCheckpoint = environment.pushedAudioPts().size();
        releaseCheckpoint.store(true, std::memory_order_release);
        QCOMPARE(audioCountAtCheckpoint, qsizetype(1));
        controller.discard();
    }

    void queuedStopRunsBeforeAnEntireFullVideoQueueIsDrained()
    {
        FakeEnvironment environment;
        environment.blockPipelineStart.store(true, std::memory_order_release);
        environment.blockVideoPush.store(true, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_VERIFY(environment.pipelineStartEntered.load(std::memory_order_acquire));
        for (qsizetype index = 0; index < RecordingController::VideoQueueCapacity; ++index) {
            GstSample *video = makeVideoSample(
                640, 360, static_cast<GstClockTime>(index + 1) * GST_SECOND);
            QVERIFY(controller.tryEnqueueVideoSample(video));
            gst_sample_unref(video);
        }
        environment.releasePipelineStart.store(true, std::memory_order_release);
        QTRY_VERIFY(environment.videoPushEntered.load(std::memory_order_acquire));

        controller.stop();
        environment.releaseVideoPush.store(true, std::memory_order_release);
        QTRY_COMPARE(controller.state(), RecordingState::Idle);
        QVERIFY(environment.videoPushCountAtWorkerFinalizeEntry.load(
                    std::memory_order_acquire) <
                RecordingController::VideoQueueCapacity);
        QCOMPARE(environment.videoPushCountAtFinalize.load(std::memory_order_acquire),
                 int(RecordingController::VideoQueueCapacity));
    }

    void synchronousDiscardRunsBeforeAnEntireFullVideoQueueIsDrained()
    {
        FakeEnvironment environment;
        environment.blockPipelineStart.store(true, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_VERIFY(environment.pipelineStartEntered.load(std::memory_order_acquire));
        for (qsizetype index = 0; index < RecordingController::VideoQueueCapacity; ++index) {
            GstSample *video = makeVideoSample(
                640, 360, static_cast<GstClockTime>(index + 1) * GST_SECOND);
            QVERIFY(controller.tryEnqueueVideoSample(video));
            gst_sample_unref(video);
        }
        environment.afterVideoPush = [&controller] {
            while (controller.state() != RecordingState::Idle) {
                QThread::yieldCurrentThread();
            }
        };
        environment.releasePipelineStart.store(true, std::memory_order_release);
        QTRY_VERIFY(environment.videoPushEntered.load(std::memory_order_acquire));

        controller.discard();
        QCOMPARE(controller.state(), RecordingState::Idle);
        QVERIFY(environment.videoPushCountAtAbort.load(std::memory_order_acquire) <
                RecordingController::VideoQueueCapacity);
    }

    void blackFramesStartAtOneSecondAndRealVideoResetsPause()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *first = makeVideoSample(640, 360, 10 * GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_COMPARE(environment.startCount(), 1);

        environment.monotonicNow.store(999'000'000, std::memory_order_release);
        QTest::qWait(80);
        QVERIFY(environment.pushedBlackPts().isEmpty());
        QCOMPARE(controller.state(), RecordingState::Recording);

        environment.monotonicNow.store(1'000'000'000, std::memory_order_release);
        QTRY_COMPARE(environment.pushedBlackPts(), QVector<qint64>{1'000'000'000});
        environment.monotonicNow.store(1'100'000'000, std::memory_order_release);
        GstSample *resumed = makeVideoSample(640, 360, 11'100'000'000);
        QVERIFY(controller.tryEnqueueVideoSample(resumed));
        gst_sample_unref(resumed);
        QTRY_COMPARE(environment.lastVideoPts(), qint64(1'100'000'000));
        const qsizetype blackCount = environment.pushedBlackPts().size();

        environment.monotonicNow.store(2'099'000'000, std::memory_order_release);
        QTest::qWait(80);
        QCOMPARE(environment.pushedBlackPts().size(), blackCount);
        QCOMPARE(controller.state(), RecordingState::Recording);
        controller.discard();
    }

    void queuedVideoUsesTapArrivalInsteadOfWorkerConsumptionTime()
    {
        FakeEnvironment environment;
        environment.blockPipelineStart.store(true, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        environment.monotonicNow.store(0, std::memory_order_release);
        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_VERIFY(environment.pipelineStartEntered.load(std::memory_order_acquire));

        environment.monotonicNow.store(100'000'000, std::memory_order_release);
        GstSample *second = makeVideoSample(640, 360, 100'000'000);
        QVERIFY(controller.tryEnqueueVideoSample(second));
        gst_sample_unref(second);
        environment.monotonicNow.store(2 * GST_SECOND, std::memory_order_release);
        environment.releasePipelineStart.store(true, std::memory_order_release);

        QTRY_COMPARE(environment.pushedVideoPts(), QVector<qint64>{100'000'000});
        QTRY_VERIFY(!environment.pushedBlackPts().isEmpty());
        QCOMPARE(environment.pushedBlackPts().constFirst(), qint64(1'100'000'000));
        controller.discard();
    }

    void tapClockExceptionDropsVideoWithoutFailingSession()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy failedSpy(&controller, &RecordingController::failed);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        environment.throwFromMonotonicClock.store(true, std::memory_order_release);
        GstSample *dropped = makeVideoSample(640, 360, 0);
        QVERIFY(!controller.tryEnqueueVideoSample(dropped));
        gst_sample_unref(dropped);
        QCOMPARE(controller.state(), RecordingState::Recording);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(environment.startCount(), 0);

        environment.throwFromMonotonicClock.store(false, std::memory_order_release);
        GstSample *accepted = makeVideoSample(640, 360, GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(accepted));
        gst_sample_unref(accepted);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.discard();
    }

    void callbackFromDiscardedSessionCannotEnterNewRecording()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        environment.monotonicClockEntered.store(false, std::memory_order_release);
        environment.blockMonotonicClock.store(true, std::memory_order_release);
        std::atomic_bool oldAccepted{true};
        std::thread oldCallback([&] {
            GstSample *oldVideo = makeVideoSample(640, 360, GST_SECOND);
            oldAccepted.store(controller.tryEnqueueVideoSample(oldVideo),
                              std::memory_order_release);
            gst_sample_unref(oldVideo);
        });
        while (!environment.monotonicClockEntered.load(std::memory_order_acquire)) {
            QThread::yieldCurrentThread();
        }

        controller.discard();
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        environment.releaseMonotonicClock.store(true, std::memory_order_release);
        oldCallback.join();
        QVERIFY(!oldAccepted.load(std::memory_order_acquire));
        QCOMPARE(environment.startCount(), 0);

        GstSample *freshVideo = makeVideoSample(640, 360, 2 * GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(freshVideo));
        gst_sample_unref(freshVideo);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.discard();
    }

    void dimensionChangeFinalizesValidPortionWithPathWarning()
    {
        FakeEnvironment environment;
        environment.reservation = RecordingFileReservationResult{
            RecordingFileReservation{QStringLiteral("video.part"), QStringLiteral("audio.part"),
                                     QStringLiteral("mp4.part"),
                                     QStringLiteral("C:/recordings/final.mp4")}, {}};
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QSignalSpy stateSpy(&controller, &RecordingController::stateChanged);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_COMPARE(environment.startCount(), 1);

        GstSample *changed = makeVideoSample(800, 600, GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(changed));
        gst_sample_unref(changed);

        QTRY_COMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 1);
        const RecordingResult result = qvariant_cast<RecordingResult>(
            finishedSpy.constFirst().constFirst());
        QCOMPARE(result.finalPath, QStringLiteral("C:/recordings/final.mp4"));
        QVERIFY(!result.warning.isEmpty());
        QVERIFY(result.warning.contains(result.finalPath));
        QCOMPARE(qvariant_cast<RecordingState>(stateSpy.at(1).at(0)),
                 RecordingState::Finalizing);
    }

    void dimensionChangeDropsEverySampleQueuedAfterBoundary()
    {
        FakeEnvironment environment;
        environment.blockPipelineStart.store(true, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);

        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_VERIFY(environment.pipelineStartEntered.load(std::memory_order_acquire));

        GstSample *changed = makeVideoSample(800, 600, GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(changed));
        gst_sample_unref(changed);
        GstSample *afterBoundary = makeVideoSample(640, 360, 2 * GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(afterBoundary));
        gst_sample_unref(afterBoundary);

        environment.releasePipelineStart.store(true, std::memory_order_release);
        QTRY_COMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(environment.pushedVideoPts(), QVector<qint64>{});
        QCOMPARE(environment.finalizeCount(), 1);
        QCOMPARE(finishedSpy.count(), 1);
        const RecordingResult result = qvariant_cast<RecordingResult>(
            finishedSpy.constFirst().constFirst());
        QVERIFY(!result.warning.isEmpty());
    }

    void dimensionBoundaryRejectsProducerAlreadyInsideTapCallback()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(controller.tryEnqueueVideoSample(first));
        gst_sample_unref(first);
        QTRY_COMPARE(environment.startCount(), 1);

        environment.blockVideoPush.store(true, std::memory_order_release);
        GstSample *beforeBoundary = makeVideoSample(640, 360, GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(beforeBoundary));
        gst_sample_unref(beforeBoundary);
        QTRY_VERIFY(environment.videoPushEntered.load(std::memory_order_acquire));
        GstSample *changed = makeVideoSample(800, 600, 2 * GST_SECOND);
        QVERIFY(controller.tryEnqueueVideoSample(changed));
        gst_sample_unref(changed);

        environment.monotonicClockEntered.store(false, std::memory_order_release);
        environment.blockMonotonicClock.store(true, std::memory_order_release);
        std::atomic_bool inFlightAccepted{true};
        std::thread producer([&] {
            GstSample *afterBoundary = makeVideoSample(640, 360, 3 * GST_SECOND);
            inFlightAccepted.store(controller.tryEnqueueVideoSample(afterBoundary),
                                   std::memory_order_release);
            gst_sample_unref(afterBoundary);
        });
        while (!environment.monotonicClockEntered.load(std::memory_order_acquire)) {
            QThread::yieldCurrentThread();
        }

        environment.releaseVideoPush.store(true, std::memory_order_release);
        QTRY_COMPARE(controller.state(), RecordingState::Idle);
        environment.releaseMonotonicClock.store(true, std::memory_order_release);
        producer.join();

        QVERIFY(!inFlightAccepted.load(std::memory_order_acquire));
        QCOMPARE(environment.pushedVideoPts(), QVector<qint64>{GST_SECOND});
        QCOMPARE(environment.finalizeCount(), 1);
    }

    void successfulFinalIsRelinquishedAndFinalizeUsesGlobalDeadline()
    {
        FakeEnvironment environment;
        {
            RecordingController controller(30, environment.hooks());
            observeVideo(controller);
            QSignalSpy finishedSpy(&controller, &RecordingController::finished);
            QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
            GstSample *video = makeVideoSample();
            QVERIFY(controller.tryEnqueueVideoSample(video));
            gst_sample_unref(video);
            QTRY_COMPARE(environment.startCount(), 1);
            controller.stop();
            QTRY_COMPARE(finishedSpy.count(), 1);
            QCOMPARE(environment.observedFinalizeDeadline.load(std::memory_order_acquire),
                     RecordingController::FinalizeDeadlineMilliseconds);
            controller.acknowledgeResult();
        }
        QCOMPARE(environment.removedFinals(), QStringList{});
    }

    void idleDiscardAfterFinishedSignalDeletesUnacknowledgedOwnedFinal()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_COMPARE(finishedSpy.count(), 1);
        QCOMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(environment.removedFinals(), QStringList{});

        controller.discard();

        QCOMPARE(environment.successfulRemovals(),
                 QStringList{environment.reservation.reservation->finalPath});
    }

    void acknowledgeFinishedResultPreservesFinalAcrossIdleDiscard()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_COMPARE(finishedSpy.count(), 1);

        controller.acknowledgeResult();
        controller.discard();

        QCOMPARE(environment.removedFinals(), QStringList{});
    }

    void acknowledgeCannotForgetOwnedFinalAfterDiscardRemovalFailure()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_COMPARE(finishedSpy.count(), 1);
        environment.removeOwnedFinalFailuresRemaining.store(2, std::memory_order_release);

        controller.discard();
        controller.acknowledgeResult();
        controller.discard();

        QCOMPARE(environment.successfulRemovals(),
                 QStringList{environment.reservation.reservation->finalPath});
    }

    void newStartImplicitlyAcknowledgesSuccessfulPriorResult()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_COMPARE(finishedSpy.count(), 1);

        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        controller.discard();

        QCOMPARE(environment.removedFinals(), QStringList{});
    }

    void commitFailureDiscardsOwnedTempsAndEmitsNoFinishedResult()
    {
        FakeEnvironment environment;
        environment.commitError = QStringLiteral("injected rename failure");
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QSignalSpy failedSpy(&controller, &RecordingController::failed);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_COMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(environment.discardCalls.load(std::memory_order_acquire), 1);
        QCOMPARE(environment.removedFinals(), QStringList{});
    }

    void discardRacingSuccessfulCommitDeletesOnlyExplicitOwnedFinal()
    {
        FakeEnvironment environment;
        environment.blockCommit.store(true, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QSignalSpy failedSpy(&controller, &RecordingController::failed);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_VERIFY(environment.commitEntered.load(std::memory_order_acquire));

        std::thread release([&environment] {
            QThread::msleep(25);
            environment.releaseCommit.store(true, std::memory_order_release);
        });
        controller.discard();
        release.join();

        QCOMPARE(controller.state(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(environment.removedFinals(),
                 QStringList{environment.reservation.reservation->finalPath});
    }

    void failedOwnedFinalRemovalRetriesWithoutForgettingOwnership()
    {
        FakeEnvironment environment;
        environment.blockCommit.store(true, std::memory_order_release);
        environment.removeOwnedFinalFailuresRemaining.store(1, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_VERIFY(environment.commitEntered.load(std::memory_order_acquire));

        std::thread releaseCommit([&] {
            while (controller.state() != RecordingState::Idle) {
                QThread::yieldCurrentThread();
            }
            environment.releaseCommit.store(true, std::memory_order_release);
        });
        controller.discard();
        releaseCommit.join();

        QCOMPARE(environment.removedFinals().size(), 2);
        QCOMPARE(environment.successfulRemovals(),
                 QStringList{environment.reservation.reservation->finalPath});
    }

    void idleDiscardRetriesOwnedFinalAfterPersistentRemovalExceptions()
    {
        FakeEnvironment environment;
        environment.blockCommit.store(true, std::memory_order_release);
        environment.removeOwnedFinalThrowsRemaining.store(10, std::memory_order_release);
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        QVERIFY(controller.start({QStringLiteral("C:/recordings"), RecordingFormat::Mp4}).accepted);
        GstSample *video = makeVideoSample();
        QVERIFY(controller.tryEnqueueVideoSample(video));
        gst_sample_unref(video);
        QTRY_COMPARE(environment.startCount(), 1);
        controller.stop();
        QTRY_VERIFY(environment.commitEntered.load(std::memory_order_acquire));

        std::thread releaseCommit([&] {
            while (controller.state() != RecordingState::Idle) {
                QThread::yieldCurrentThread();
            }
            environment.releaseCommit.store(true, std::memory_order_release);
        });
        controller.discard();
        releaseCommit.join();
        const qsizetype failedAttempts = environment.removedFinals().size();
        QVERIFY(failedAttempts >= 2);
        QVERIFY(failedAttempts <= 4);
        QVERIFY(environment.successfulRemovals().isEmpty());

        const int discardsBeforeRetry = environment.discardCalls.load(
            std::memory_order_acquire);
        const RecordingStartResult retry = controller.start(
            {QStringLiteral("C:/recordings"), RecordingFormat::Mp4});
        QVERIFY(!retry.accepted);
        QCOMPARE(environment.discardCalls.load(std::memory_order_acquire),
                 discardsBeforeRetry + 1);

        environment.removeOwnedFinalThrowsRemaining.store(0, std::memory_order_release);
        controller.discard();
        QVERIFY(environment.removedFinals().size() > failedAttempts);
        QCOMPARE(environment.successfulRemovals(),
                 QStringList{environment.reservation.reservation->finalPath});
    }

    void staleCleanupRunsOnceForEachNewOutputDirectory()
    {
        FakeEnvironment environment;
        RecordingController controller(30, environment.hooks());
        observeVideo(controller);
        const RecordingOptions first{QStringLiteral("C:/recordings-a"), RecordingFormat::Mp4};
        const RecordingOptions second{QStringLiteral("C:/recordings-b"), RecordingFormat::Mp4};

        QVERIFY(controller.start(first).accepted);
        controller.discard();
        QVERIFY(controller.start(first).accepted);
        controller.discard();
        QCOMPARE(environment.cleanupCalls, 1);
        QVERIFY(controller.start(second).accepted);
        controller.discard();
        QCOMPARE(environment.cleanupCalls, 2);
    }

    void realControllerLateAudioCommitsDiscoverableMp4()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RecordingController controller;
        GstSample *observed = makeVideoSample(640, 360, 0);
        observeVideo(controller, observed);
        const RecordingStartResult startResult = controller.start(
            {directory.path(), RecordingFormat::Mp4});
        QVERIFY2(startResult.accepted, qPrintable(startResult.error));
        QSignalSpy finishedSpy(&controller, &RecordingController::finished);
        QSignalSpy failedSpy(&controller, &RecordingController::failed);

        auto enqueueVideo = [&controller](GstSample *sample) {
            QElapsedTimer deadline;
            deadline.start();
            while (!controller.tryEnqueueVideoSample(sample) && deadline.elapsed() < 2'000) {
                QTest::qWait(5);
            }
            return deadline.elapsed() < 2'000;
        };
        auto enqueueAudio = [&controller](GstSample *sample) {
            QElapsedTimer deadline;
            deadline.start();
            while (!controller.tryEnqueueAudioSample(sample) && deadline.elapsed() < 2'000) {
                QTest::qWait(5);
            }
            return deadline.elapsed() < 2'000;
        };

        GstSample *first = makeVideoSample(640, 360, 0);
        QVERIFY(enqueueVideo(first));
        gst_sample_unref(first);
        QTest::qWait(750);
        for (int frame = 1; frame <= 60; ++frame) {
            GstSample *video = makeVideoSample(
                640, 360, gst_util_uint64_scale(frame, GST_SECOND, 30));
            QVERIFY(enqueueVideo(video));
            gst_sample_unref(video);
            if (frame >= 36) {
                GstSample *audio = makeAudioSample(
                    gst_util_uint64_scale(frame, GST_SECOND, 30));
                QVERIFY(enqueueAudio(audio));
                gst_sample_unref(audio);
            }
        }
        controller.stop();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 45'000);
        QCOMPARE(failedSpy.count(), 0);
        const RecordingResult result = qvariant_cast<RecordingResult>(
            finishedSpy.constFirst().constFirst());
        QVERIFY(QFileInfo::exists(result.finalPath));
        QVERIFY(QFileInfo(result.finalPath).size() > 0);
        MediaCounts counts;
        QString discoverError;
        QVERIFY2(discoverCounts(result.finalPath, &counts, &discoverError),
                 qPrintable(discoverError));
        QCOMPARE(counts.video, 1);
        QCOMPARE(counts.audio, 1);
    }
};

QTEST_GUILESS_MAIN(RecordingControllerTest)
#include "RecordingControllerTest.moc"

#include "backend/GstRecordingPipeline.h"
#include "backend/RecordingFileTransaction.h"

#include <QtTest>

#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>

#include <cmath>
#include <atomic>
#include <future>
#include <type_traits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

std::atomic<int> g_asyncProbeFinalizations{0};

void countAsyncProbeFinalization(gpointer, GObject *)
{
    g_asyncProbeFinalizations.fetch_add(1, std::memory_order_relaxed);
}

const QSet<QString> kRequiredFactories{
    QStringLiteral("matroskamux"), QStringLiteral("matroskademux"),
    QStringLiteral("mp4mux"), QStringLiteral("h264parse"),
    QStringLiteral("avenc_aac"), QStringLiteral("aacparse"),
    QStringLiteral("appsrc"), QStringLiteral("appsink"),
    QStringLiteral("videoconvert"), QStringLiteral("audioconvert"),
    QStringLiteral("audioresample"), QStringLiteral("capsfilter"),
    QStringLiteral("filesrc"), QStringLiteral("filesink"),
    QStringLiteral("identity"), QStringLiteral("mfh264enc"),
    QStringLiteral("openh264enc")};

GstRecordingPipelineHooks availableHooks(QSet<QString> factories = kRequiredFactories)
{
    GstRecordingPipelineHooks hooks;
    hooks.factoryExists = [factories = std::move(factories)](const QString &name) {
        return factories.contains(name);
    };
    hooks.factoryReady = [](const QString &, QString *) { return true; };
    return hooks;
}

GstSample *makeVideoSample(int width, int height, GstClockTime pts, int requestedStride = 0)
{
    const int stride = requestedStride > 0 ? requestedStride : width * 4;
    GstBuffer *buffer = gst_buffer_new_allocate(
        nullptr, static_cast<gsize>(stride) * static_cast<gsize>(height), nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    if (requestedStride > 0) {
        gsize offsets[GST_VIDEO_MAX_PLANES]{};
        gint strides[GST_VIDEO_MAX_PLANES]{};
        strides[0] = stride;
        gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                       GST_VIDEO_FORMAT_RGBA, width, height, 1,
                                       offsets, strides);
    }
    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "RGBA",
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, 30, 1,
        "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

GstSample *makeAudioSample(GstClockTime pts, const char *format = "S16LE",
                           int rate = 44100, int channels = 2)
{
    constexpr gsize frames = 441;
    GstBuffer *buffer = gst_buffer_new_allocate(
        nullptr, frames * static_cast<gsize>(channels) * sizeof(gint16), nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
    GstCaps *caps = gst_caps_new_simple(
        "audio/x-raw", "format", G_TYPE_STRING, format,
        "rate", G_TYPE_INT, rate, "channels", G_TYPE_INT, channels,
        "layout", G_TYPE_STRING, "interleaved", nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

struct ReservedFiles {
    QTemporaryDir directory;
    RecordingFileReservation reservation;
    bool committed = false;

    ReservedFiles()
    {
        const RecordingFileReservationResult result = RecordingFileTransaction::reserve(
            directory.path(), QDateTime(QDate(2026, 7, 14), QTime(10, 0, 0)),
            QUuid(QStringLiteral("{c6efdf7e-b4b2-4474-a46d-1a5974eb1d12}")));
        if (result.reservation) reservation = *result.reservation;
    }

    ~ReservedFiles()
    {
        if (!committed) RecordingFileTransaction::discard(reservation);
    }

    QString commit()
    {
        const QString error = RecordingFileTransaction::commit(reservation);
        if (error.isEmpty()) committed = true;
        return error;
    }
};

GstRecordingPipelineConfig configFor(const RecordingFileReservation &reservation)
{
    GstRecordingPipelineConfig config;
    config.videoSpoolPath = reservation.videoSpoolPath;
    config.audioSpoolPath = reservation.audioSpoolPath;
    config.temporaryMp4Path = reservation.temporaryMp4Path;
    config.video = {640, 360, 30, 1, 1, 1};
    config.videoBitrateBitsPerSecond = 4'000'000;
    config.preferredEncoder = QStringLiteral("openh264enc");
    return config;
}

bool isHiddenFile(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(QDir::toNativeSeparators(path).toStdWString().c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
    Q_UNUSED(path);
    return true;
#endif
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
        *error = QString::fromUtf8(createError ? createError->message : "discoverer creation failed");
        g_clear_error(&createError);
        return false;
    }
    GError *discoverError = nullptr;
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri.constData(), &discoverError);
    if (!info || discoverError) {
        *error = QString::fromUtf8(discoverError ? discoverError->message : "discovery failed");
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

struct DecodeTargets {
    GstElement *video = nullptr;
    GstElement *audio = nullptr;
};

void linkDecodedPad(GstElement *, GstPad *pad, gpointer data)
{
    auto *targets = static_cast<DecodeTargets *>(data);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const char *name = caps && !gst_caps_is_empty(caps)
        ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : nullptr;
    GstElement *sink = g_strcmp0(name, "video/x-raw") == 0 ? targets->video
        : (g_strcmp0(name, "audio/x-raw") == 0 ? targets->audio : nullptr);
    if (sink) {
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        if (!gst_pad_is_linked(sinkPad)) gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
    }
    if (caps) gst_caps_unref(caps);
}

GstClockTime pullRunningTime(GstElement *sink)
{
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
    if (!sample) return GST_CLOCK_TIME_NONE;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    const GstSegment *segment = gst_sample_get_segment(sample);
    const GstClockTime pts = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
    const GstClockTime running = segment && GST_CLOCK_TIME_IS_VALID(pts)
        ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, pts) : GST_CLOCK_TIME_NONE;
    gst_sample_unref(sample);
    return running;
}

bool firstDecodedRunningTimes(const QString &path, GstClockTime *video,
                              GstClockTime *audio, QString *error)
{
    GstElement *pipeline = gst_pipeline_new("recording_test_decode");
    GstElement *decode = gst_element_factory_make("uridecodebin", nullptr);
    GstElement *videoSink = gst_element_factory_make("appsink", nullptr);
    GstElement *audioSink = gst_element_factory_make("appsink", nullptr);
    if (!pipeline || !decode || !videoSink || !audioSink) {
        *error = QStringLiteral("decode test factories missing");
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    g_object_set(decode, "uri", uri.constData(), nullptr);
    g_object_set(videoSink, "sync", FALSE, "max-buffers", 1u, "drop", FALSE, nullptr);
    g_object_set(audioSink, "sync", FALSE, "max-buffers", 1u, "drop", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), decode, videoSink, audioSink, nullptr);
    DecodeTargets targets{videoSink, audioSink};
    g_signal_connect(decode, "pad-added", G_CALLBACK(linkDecodedPad), &targets);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *error = QStringLiteral("decode test could not enter PLAYING");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    auto videoFuture = std::async(std::launch::async, [&] { return pullRunningTime(videoSink); });
    auto audioFuture = std::async(std::launch::async, [&] { return pullRunningTime(audioSink); });
    *video = videoFuture.get();
    *audio = audioFuture.get();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    if (!GST_CLOCK_TIME_IS_VALID(*video) || !GST_CLOCK_TIME_IS_VALID(*audio)) {
        *error = QStringLiteral("decode test did not receive both tracks");
        return false;
    }
    return true;
}

GstMessage *injectedErrorMessage(GstBus *bus, const QString &message)
{
    const QByteArray utf8 = message.toUtf8();
    GError *error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
                                        utf8.constData());
    GstMessage *result = gst_message_new_error(GST_OBJECT(bus), error,
                                               "injected pipeline failure");
    g_error_free(error);
    return result;
}

} // namespace

class GstRecordingPipelineTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void capabilityProbeSelectsEncoderInPriorityOrder();
    void capabilityProbeFallsBackAfterMissingOrUnreadyMfh();
    void capabilityProbeClassifiesMissingFactories_data();
    void capabilityProbeClassifiesMissingFactories();
    void realCapabilityProbeUsesInstalledOpenH264Fallback();
    void realReadyProbeRejectsAsyncTimeoutAndReleasesElement_data();
    void realReadyProbeRejectsAsyncTimeoutAndReleasesElement();
    void startRejectsMissingRequiredCapabilityWithoutTouchingPlaceholder_data();
    void startRejectsMissingRequiredCapabilityWithoutTouchingPlaceholder();
    void startSkipsEncoderThatFailsReadyProbe();
    void startTriesValidPreferredEncoderFirstWithoutDuplicates();
    void videoStartRebuildsCleanlyFromMfhToOpenH264();
    void videoFallbackRebuildsAfterEveryMfhFailureStage_data();
    void videoFallbackRebuildsAfterEveryMfhFailureStage();
    void encoderConfigurationUsesFactorySpecificUnitsAndFormats();
    void videoGraphUsesRequiredCapsAndEncoderUnits();
    void realVideoStartUsesFirstRunnableEncoder();
    void videoOnlyFinalizeCreatesHiddenMp4WithoutAudioBranch();
    void lateAudioCreatesClaimedHiddenSpoolAndPreservesOffset();
    void audioClaimFailureDoesNotOverwriteForeignFile();
    void blackFrameUsesLockedStrideAndDoesNotMutateBorrowedBuffers();
    void invalidAudioAndDimensionChangeAreRejectedWithoutPush();
    void injectedDeadlineAndCancellationAbortPromptly();
    void injectedRemuxBusErrorIsSpecificAndReleasesPipeline();
    void injectedTrackAndRemuxTimeouts_data();
    void injectedTrackAndRemuxTimeouts();
    void injectedPipelineBusErrorsAreClassified_data();
    void injectedPipelineBusErrorsAreClassified();
};

void GstRecordingPipelineTest::initTestCase()
{
    gst_init(nullptr, nullptr);
}

void GstRecordingPipelineTest::capabilityProbeSelectsEncoderInPriorityOrder()
{
    const GstRecordingCapabilityResult result =
        GstRecordingPipeline::probeCapabilities(availableHooks());
    QVERIFY2(result.available, qPrintable(result.error));
    QCOMPARE(result.preferredEncoder, QStringLiteral("mfh264enc"));
}

void GstRecordingPipelineTest::capabilityProbeFallsBackAfterMissingOrUnreadyMfh()
{
    QSet<QString> withoutMfh = kRequiredFactories;
    withoutMfh.remove(QStringLiteral("mfh264enc"));
    GstRecordingCapabilityResult result =
        GstRecordingPipeline::probeCapabilities(availableHooks(withoutMfh));
    QVERIFY2(result.available, qPrintable(result.error));
    QCOMPARE(result.preferredEncoder, QStringLiteral("openh264enc"));

    GstRecordingPipelineHooks hooks = availableHooks();
    QStringList readyAttempts;
    hooks.factoryReady = [&readyAttempts](const QString &name, QString *error) {
        readyAttempts.append(name);
        if (name == QStringLiteral("mfh264enc")) {
            *error = QStringLiteral("injected READY failure");
            return false;
        }
        return true;
    };
    result = GstRecordingPipeline::probeCapabilities(hooks);
    QVERIFY2(result.available, qPrintable(result.error));
    QCOMPARE(result.preferredEncoder, QStringLiteral("openh264enc"));
    QCOMPARE(readyAttempts,
             QStringList({QStringLiteral("mfh264enc"), QStringLiteral("openh264enc")}));

    hooks.factoryReady = [](const QString &, QString *error) {
        *error = QStringLiteral("injected READY failure");
        return false;
    };
    result = GstRecordingPipeline::probeCapabilities(hooks);
    QVERIFY(!result.available);
    QVERIFY(result.error.contains(QStringLiteral("H.264 encoder")));
    QVERIFY(result.error.contains(QStringLiteral("READY")));
}

void GstRecordingPipelineTest::capabilityProbeClassifiesMissingFactories_data()
{
    QTest::addColumn<QString>("factory");
    QTest::addColumn<QString>("classification");
    QTest::newRow("matroskamux") << QStringLiteral("matroskamux") << QStringLiteral("Matroska spool");
    QTest::newRow("matroskademux") << QStringLiteral("matroskademux") << QStringLiteral("Matroska spool");
    QTest::newRow("mp4mux") << QStringLiteral("mp4mux") << QStringLiteral("MP4 muxer");
    QTest::newRow("h264parse") << QStringLiteral("h264parse") << QStringLiteral("h264parse");
    QTest::newRow("avenc_aac") << QStringLiteral("avenc_aac") << QStringLiteral("AAC encoder");
    QTest::newRow("aacparse") << QStringLiteral("aacparse") << QStringLiteral("aacparse");
    QTest::newRow("appsrc") << QStringLiteral("appsrc") << QStringLiteral("appsrc");
    QTest::newRow("appsink") << QStringLiteral("appsink") << QStringLiteral("appsink");
    QTest::newRow("videoconvert") << QStringLiteral("videoconvert") << QStringLiteral("videoconvert");
    QTest::newRow("audioconvert") << QStringLiteral("audioconvert") << QStringLiteral("audioconvert");
    QTest::newRow("audioresample") << QStringLiteral("audioresample") << QStringLiteral("audioresample");
    QTest::newRow("capsfilter") << QStringLiteral("capsfilter") << QStringLiteral("capsfilter");
    QTest::newRow("filesrc") << QStringLiteral("filesrc") << QStringLiteral("filesrc");
    QTest::newRow("filesink") << QStringLiteral("filesink") << QStringLiteral("filesink");
    QTest::newRow("identity") << QStringLiteral("identity") << QStringLiteral("identity");
}

void GstRecordingPipelineTest::capabilityProbeClassifiesMissingFactories()
{
    QFETCH(QString, factory);
    QFETCH(QString, classification);
    QSet<QString> factories = kRequiredFactories;
    factories.remove(factory);
    const GstRecordingCapabilityResult result =
        GstRecordingPipeline::probeCapabilities(availableHooks(factories));
    QVERIFY(result.error.contains(classification));
    QVERIFY(result.error.contains(factory));
    QVERIFY(!result.available);
}

void GstRecordingPipelineTest::realCapabilityProbeUsesInstalledOpenH264Fallback()
{
    GstElementFactory *openH264 = gst_element_factory_find("openh264enc");
    QVERIFY(openH264 != nullptr);
    gst_object_unref(openH264);
    GstRecordingPipelineHooks hooks;
    hooks.factoryExists = [](const QString &name) {
        if (name == QStringLiteral("mfh264enc")) return false;
        GstElementFactory *factory = gst_element_factory_find(name.toUtf8().constData());
        if (!factory) return false;
        gst_object_unref(factory);
        return true;
    };
    const GstRecordingCapabilityResult result = GstRecordingPipeline::probeCapabilities(hooks);
    QVERIFY2(result.available, qPrintable(result.error));
    QCOMPARE(result.preferredEncoder, QStringLiteral("openh264enc"));
}

void GstRecordingPipelineTest::realReadyProbeRejectsAsyncTimeoutAndReleasesElement()
{
    QFETCH(int, requestedValue);
    QFETCH(int, settledValue);
    QFETCH(int, settledStateValue);
    QFETCH(bool, expectedAvailable);
    QFETCH(QString, errorToken);
    GstRecordingPipelineHooks hooks;
    hooks.factoryExists = [](const QString &name) {
        return name != QStringLiteral("airplaymissingprobe");
    };
    hooks.encoderFactoryAlias = [](const QString &logical) {
        return logical == QStringLiteral("mfh264enc")
            ? QStringLiteral("fakesink")
            : QStringLiteral("airplaymissingprobe");
    };
    hooks.factoryReadyDeadline = 5 * GST_MSECOND;
    GstClockTime observedDeadline = GST_CLOCK_TIME_NONE;
    hooks.factoryReadySetState = [requestedValue](GstElement *, GstState state) {
        return state == GST_STATE_READY
            ? static_cast<GstStateChangeReturn>(requestedValue)
            : GST_STATE_CHANGE_FAILURE;
    };
    hooks.factoryReadyGetState = [&observedDeadline, settledValue, settledStateValue]
        (GstElement *, GstState *state, GstState *pending, GstClockTime deadline)
        -> GstStateChangeReturn {
        observedDeadline = deadline;
        *state = static_cast<GstState>(settledStateValue);
        *pending = GST_STATE_READY;
        return static_cast<GstStateChangeReturn>(settledValue);
    };
    hooks.factoryReadyElementCreated = [](GstElement *element) {
        g_object_weak_ref(G_OBJECT(element), countAsyncProbeFinalization, nullptr);
    };
    const int finalizedBefore = g_asyncProbeFinalizations.load(std::memory_order_relaxed);
    QElapsedTimer timer;
    timer.start();
    const GstRecordingCapabilityResult result =
        GstRecordingPipeline::probeCapabilities(hooks);
    QCOMPARE(result.available, expectedAvailable);
    if (expectedAvailable) {
        QCOMPARE(result.preferredEncoder, QStringLiteral("mfh264enc"));
        QVERIFY(result.error.isEmpty());
    } else {
        QVERIFY(result.error.contains(QStringLiteral("READY")));
        QVERIFY(result.error.contains(errorToken, Qt::CaseInsensitive));
    }
    if (requestedValue == GST_STATE_CHANGE_FAILURE) {
        QCOMPARE(observedDeadline, GST_CLOCK_TIME_NONE);
    } else {
        QCOMPARE(observedDeadline, 5 * GST_MSECOND);
    }
    QVERIFY(timer.elapsed() < 200);
    QCOMPARE(g_asyncProbeFinalizations.load(std::memory_order_relaxed),
             finalizedBefore + 1);
}

void GstRecordingPipelineTest::realReadyProbeRejectsAsyncTimeoutAndReleasesElement_data()
{
    QTest::addColumn<int>("requestedValue");
    QTest::addColumn<int>("settledValue");
    QTest::addColumn<int>("settledStateValue");
    QTest::addColumn<bool>("expectedAvailable");
    QTest::addColumn<QString>("errorToken");
    QTest::newRow("async-timeout")
        << int(GST_STATE_CHANGE_ASYNC) << int(GST_STATE_CHANGE_ASYNC)
        << int(GST_STATE_NULL) << false << QStringLiteral("timeout");
    QTest::newRow("async-settles-ready")
        << int(GST_STATE_CHANGE_ASYNC) << int(GST_STATE_CHANGE_SUCCESS)
        << int(GST_STATE_READY) << true << QString();
    QTest::newRow("async-settles-non-ready")
        << int(GST_STATE_CHANGE_ASYNC) << int(GST_STATE_CHANGE_SUCCESS)
        << int(GST_STATE_PAUSED) << false << QStringLiteral("non-READY");
    QTest::newRow("settlement-failure")
        << int(GST_STATE_CHANGE_ASYNC) << int(GST_STATE_CHANGE_FAILURE)
        << int(GST_STATE_NULL) << false << QStringLiteral("settlement failed");
    QTest::newRow("request-failure")
        << int(GST_STATE_CHANGE_FAILURE) << int(GST_STATE_CHANGE_SUCCESS)
        << int(GST_STATE_READY) << false << QStringLiteral("request failed");
}

void GstRecordingPipelineTest::startRejectsMissingRequiredCapabilityWithoutTouchingPlaceholder_data()
{
    QTest::addColumn<QString>("missingFactory");
    QTest::addColumn<QString>("classification");
    QTest::newRow("mp4mux") << QStringLiteral("mp4mux") << QStringLiteral("MP4 muxer");
    QTest::newRow("matroskademux") << QStringLiteral("matroskademux") << QStringLiteral("Matroska spool");
    QTest::newRow("avenc_aac") << QStringLiteral("avenc_aac") << QStringLiteral("AAC encoder");
}

void GstRecordingPipelineTest::startRejectsMissingRequiredCapabilityWithoutTouchingPlaceholder()
{
    QFETCH(QString, missingFactory);
    QFETCH(QString, classification);
    ReservedFiles files;
    QSet<QString> factories = kRequiredFactories;
    factories.remove(missingFactory);
    GstRecordingPipelineHooks hooks = availableHooks(factories);
    QStringList created;
    hooks.elementCreated = [&created](const QString &name) { created.append(name); };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY(!pipeline.start(configFor(files.reservation), first, &error));
    gst_sample_unref(first);
    QVERIFY(error.contains(classification));
    QVERIFY(error.contains(missingFactory));
    QVERIFY(created.isEmpty());
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    QCOMPARE(QFileInfo(files.reservation.videoSpoolPath).size(), qint64(0));
}

void GstRecordingPipelineTest::startSkipsEncoderThatFailsReadyProbe()
{
    ReservedFiles files;
    GstRecordingPipelineHooks hooks = availableHooks();
    hooks.factoryReady = [](const QString &name, QString *error) {
        if (name == QStringLiteral("mfh264enc")) {
            *error = QStringLiteral("injected READY failure");
            return false;
        }
        return true;
    };
    QStringList attempts;
    hooks.encoderAttempted = [&attempts](const QString &name) { attempts.append(name); };
    GstRecordingPipeline pipeline(hooks);
    GstRecordingPipelineConfig config = configFor(files.reservation);
    config.preferredEncoder = QStringLiteral("mfh264enc");
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY2(pipeline.start(config, first, &error), qPrintable(error));
    gst_sample_unref(first);
    QCOMPARE(attempts, QStringList({QStringLiteral("openh264enc")}));
    QCOMPARE(pipeline.encoderFactoryName(), QStringLiteral("openh264enc"));
    pipeline.abort();
}

void GstRecordingPipelineTest::startTriesValidPreferredEncoderFirstWithoutDuplicates()
{
    ReservedFiles files;
    GstRecordingPipelineHooks hooks = availableHooks();
    QStringList attempts;
    hooks.encoderAttempted = [&attempts](const QString &name) { attempts.append(name); };
    hooks.encoderBuildAllowed = [](const QString &name, QString *error) {
        if (name == QStringLiteral("openh264enc")) {
            *error = QStringLiteral("injected preferred failure");
            return false;
        }
        return true;
    };
    GstRecordingPipeline pipeline(hooks);
    GstRecordingPipelineConfig config = configFor(files.reservation);
    config.preferredEncoder = QStringLiteral("openh264enc");
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY2(pipeline.start(config, first, &error), qPrintable(error));
    gst_sample_unref(first);
    QCOMPARE(attempts,
             QStringList({QStringLiteral("openh264enc"), QStringLiteral("mfh264enc")}));
    QCOMPARE(attempts.count(QStringLiteral("openh264enc")), 1);
    QCOMPARE(attempts.count(QStringLiteral("mfh264enc")), 1);
    pipeline.abort();
}

void GstRecordingPipelineTest::videoStartRebuildsCleanlyFromMfhToOpenH264()
{
    ReservedFiles files;
    QVERIFY(files.directory.isValid());
    QVERIFY(!files.reservation.videoSpoolPath.isEmpty());
    QStringList attempts;
    GstRecordingPipelineHooks hooks;
    hooks.encoderAttempted = [&attempts](const QString &name) { attempts.append(name); };
    hooks.encoderBuildAllowed = [](const QString &name, QString *error) {
        if (name == QStringLiteral("mfh264enc")) {
            *error = QStringLiteral("injected mfh caps failure");
            return false;
        }
        return true;
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 7 * GST_SECOND);
    QString error;
    GstRecordingPipelineConfig config = configFor(files.reservation);
    config.preferredEncoder = QStringLiteral("mfh264enc");
    QVERIFY2(pipeline.start(config, first, &error), qPrintable(error));
    QCOMPARE(GST_BUFFER_PTS(gst_sample_get_buffer(first)), 7 * GST_SECOND);
    gst_sample_unref(first);
    QCOMPARE(attempts,
             QStringList({QStringLiteral("mfh264enc"), QStringLiteral("openh264enc")}));
    QCOMPARE(pipeline.encoderFactoryName(), QStringLiteral("openh264enc"));
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    pipeline.abort();
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
}

void GstRecordingPipelineTest::videoFallbackRebuildsAfterEveryMfhFailureStage_data()
{
    QTest::addColumn<QString>("stage");
    QTest::newRow("factory") << QStringLiteral("factory");
    QTest::newRow("caps") << QStringLiteral("caps");
    QTest::newRow("link") << QStringLiteral("link");
    QTest::newRow("ready") << QStringLiteral("ready");
    QTest::newRow("playing") << QStringLiteral("playing");
}

void GstRecordingPipelineTest::videoFallbackRebuildsAfterEveryMfhFailureStage()
{
    QFETCH(QString, stage);
    ReservedFiles files;
    QStringList attempts;
    QStringList stageHits;
    QStringList nullPipelines;
    GstRecordingPipelineHooks hooks;
    hooks.encoderAttempted = [&attempts](const QString &name) { attempts.append(name); };
    hooks.encoderFactoryAlias = [](const QString &logicalEncoder) {
        return logicalEncoder == QStringLiteral("mfh264enc")
            ? QStringLiteral("openh264enc") : logicalEncoder;
    };
    hooks.pipelineStateObserved = [&nullPipelines](const QString &pipeline, GstState state) {
        if (state == GST_STATE_NULL) nullPipelines.append(pipeline);
    };
    hooks.encoderStageAllowed = [stage, &stageHits](const QString &encoder,
                                                    const QString &candidateStage,
                                                    QString *error) {
        if (encoder == QStringLiteral("mfh264enc") && candidateStage == stage) {
            stageHits.append(candidateStage);
            *error = QStringLiteral("injected %1 failure").arg(stage);
            return false;
        }
        if (encoder == QStringLiteral("mfh264enc")) stageHits.append(candidateStage);
        return true;
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    GstRecordingPipelineConfig config = configFor(files.reservation);
    config.preferredEncoder = QStringLiteral("mfh264enc");
    QVERIFY2(pipeline.start(config, first, &error), qPrintable(error));
    gst_sample_unref(first);
    QCOMPARE(attempts,
             QStringList({QStringLiteral("mfh264enc"), QStringLiteral("openh264enc")}));
    QCOMPARE(pipeline.encoderFactoryName(), QStringLiteral("openh264enc"));
    const QStringList orderedStages{QStringLiteral("factory"), QStringLiteral("caps"),
                                    QStringLiteral("link"), QStringLiteral("ready"),
                                    QStringLiteral("playing")};
    QCOMPARE(stageHits, orderedStages.mid(0, orderedStages.indexOf(stage) + 1));
    QCOMPARE(nullPipelines.count(QStringLiteral("video-spool")), 1);
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    pipeline.abort();
    QVERIFY(nullPipelines.count(QStringLiteral("video-spool")) >= 2);
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
}

void GstRecordingPipelineTest::encoderConfigurationUsesFactorySpecificUnitsAndFormats()
{
    const GstRecordingEncoderConfiguration mfh =
        GstRecordingPipeline::encoderConfiguration(QStringLiteral("mfh264enc"), 4'000'000);
    QCOMPARE(mfh.inputFormat, QStringLiteral("NV12"));
    QCOMPARE(mfh.bitratePropertyValue, 4000);
    QCOMPARE(mfh.rateControl, QStringLiteral("cbr"));
    QVERIFY(mfh.usageType.isEmpty());

    const GstRecordingEncoderConfiguration openh264 =
        GstRecordingPipeline::encoderConfiguration(QStringLiteral("openh264enc"), 4'000'000);
    QCOMPARE(openh264.inputFormat, QStringLiteral("I420"));
    QCOMPARE(openh264.bitratePropertyValue, 4'000'000);
    QCOMPARE(openh264.rateControl, QStringLiteral("bitrate"));
    QCOMPARE(openh264.usageType, QStringLiteral("screen"));
}

void GstRecordingPipelineTest::videoGraphUsesRequiredCapsAndEncoderUnits()
{
    ReservedFiles files;
    QStringList observations;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.configurationObserved = [&observations](const QString &element,
                                                   const QString &key,
                                                   const QString &value) {
        observations.append(element + QLatin1Char(':') + key + QLatin1Char('=') + value);
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    GstRecordingPipelineConfig config = configFor(files.reservation);
    config.preferredEncoder = QStringLiteral("mfh264enc");
    QVERIFY2(pipeline.start(config, first, &error), qPrintable(error));
    gst_sample_unref(first);

    QVERIFY(observations.contains(QStringLiteral("video-appsrc:caps=video/x-raw,format=RGBA,width=640,height=360,framerate=30/1,pixel-aspect-ratio=1/1")));
    QVERIFY(observations.contains(QStringLiteral("openh264enc:bitrate=4000000")));
    QVERIFY(observations.contains(QStringLiteral("openh264enc:rate-control=bitrate")));
    QVERIFY(observations.contains(QStringLiteral("openh264enc:usage-type=screen")));
    QVERIFY(observations.contains(QStringLiteral("openh264enc:input-format=I420")));
    QVERIFY(observations.contains(QStringLiteral("h264parse:caps=video/x-h264,stream-format=avc,alignment=au")));
    QVERIFY(observations.contains(QStringLiteral("video-graph:converters=videoconvert")));
    QVERIFY(!observations.join(QLatin1Char('\n')).contains(QStringLiteral("videoscale")));
    QVERIFY(!observations.join(QLatin1Char('\n')).contains(QStringLiteral("videocrop")));
    QVERIFY(!observations.join(QLatin1Char('\n')).contains(QStringLiteral("videoflip")));
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    pipeline.abort();
}

void GstRecordingPipelineTest::realVideoStartUsesFirstRunnableEncoder()
{
    ReservedFiles files;
    QStringList attempts;
    GstRecordingPipelineHooks hooks;
    hooks.encoderAttempted = [&attempts](const QString &name) { attempts.append(name); };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    GstRecordingPipelineConfig config = configFor(files.reservation);
    config.preferredEncoder = QStringLiteral("mfh264enc");
    QVERIFY2(pipeline.start(config, first, &error), qPrintable(error));
    gst_sample_unref(first);
    if (pipeline.encoderFactoryName() == QStringLiteral("mfh264enc")) {
        QCOMPARE(attempts, QStringList({QStringLiteral("mfh264enc")}));
    } else {
        QCOMPARE(pipeline.encoderFactoryName(), QStringLiteral("openh264enc"));
        QCOMPARE(attempts,
                 QStringList({QStringLiteral("mfh264enc"), QStringLiteral("openh264enc")}));
    }
    pipeline.abort();
}

void GstRecordingPipelineTest::videoOnlyFinalizeCreatesHiddenMp4WithoutAudioBranch()
{
    ReservedFiles files;
    QStringList created;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.elementCreated = [&created](const QString &name) { created.append(name); };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY2(pipeline.start(configFor(files.reservation), first, &error), qPrintable(error));
    gst_sample_unref(first);
    for (int index = 1; index < 60; ++index) {
        GstSample *sample = makeVideoSample(640, 360, index * GST_SECOND / 30);
        QVERIFY2(pipeline.pushVideo(sample, index * GST_SECOND / 30, &error), qPrintable(error));
        gst_sample_unref(sample);
    }
    QVERIFY(!QFileInfo::exists(files.reservation.audioSpoolPath));
    QVERIFY(!created.contains(QStringLiteral("mp4mux")));
    QVERIFY(!created.contains(QStringLiteral("avenc_aac")));
    std::atomic_bool cancelled{false};
    const GstRecordingFinalizeResult result = pipeline.finalize(30'000, cancelled);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(isHiddenFile(files.reservation.temporaryMp4Path));
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    QVERIFY(!QFileInfo::exists(files.reservation.audioSpoolPath));
    QVERIFY(created.contains(QStringLiteral("mp4mux")));
    QVERIFY(!created.contains(QStringLiteral("audio_%u")));
    const QString commitError = files.commit();
    QVERIFY2(commitError.isEmpty(), qPrintable(commitError));
    QVERIFY(!QFileInfo::exists(files.reservation.videoSpoolPath));
    MediaCounts counts;
    QVERIFY2(discoverCounts(files.reservation.finalPath, &counts, &error), qPrintable(error));
    QCOMPARE(counts.video, 1);
    QCOMPARE(counts.audio, 0);
}

void GstRecordingPipelineTest::lateAudioCreatesClaimedHiddenSpoolAndPreservesOffset()
{
    ReservedFiles files;
    QStringList created;
    QStringList observations;
    QStringList nullPipelines;
    QStringList padEvents;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.elementCreated = [&created](const QString &name) { created.append(name); };
    hooks.configurationObserved = [&observations](const QString &element,
                                                   const QString &key,
                                                   const QString &value) {
        observations.append(element + QLatin1Char(':') + key + QLatin1Char('=') + value);
    };
    hooks.pipelineStateObserved = [&nullPipelines](const QString &name, GstState state) {
        if (state == GST_STATE_NULL) nullPipelines.append(name);
    };
    hooks.requestPadObserved = [&padEvents](const QString &name, bool acquired) {
        padEvents.append(name + (acquired ? QStringLiteral(":acquired")
                                          : QStringLiteral(":released")));
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY2(pipeline.start(configFor(files.reservation), first, &error), qPrintable(error));
    gst_sample_unref(first);
    QVERIFY(!QFileInfo::exists(files.reservation.audioSpoolPath));
    QVERIFY(!created.contains(QStringLiteral("mp4mux")));
    for (int index = 1; index < 90; ++index) {
        GstSample *sample = makeVideoSample(640, 360, index * GST_SECOND / 30);
        QVERIFY(pipeline.pushVideo(sample, index * GST_SECOND / 30, &error));
        gst_sample_unref(sample);
    }
    constexpr GstClockTime sourceAudioPts = 9 * GST_SECOND;
    constexpr qint64 origin = 1'200'000'000;
    for (int index = 0; index < 180; ++index) {
        GstSample *sample = makeAudioSample(sourceAudioPts + index * 10 * GST_MSECOND);
        QVERIFY2(pipeline.pushAudio(sample, origin + index * 10 * GST_MSECOND, &error),
                 qPrintable(error));
        QCOMPARE(GST_BUFFER_PTS(gst_sample_get_buffer(sample)), sourceAudioPts + index * 10 * GST_MSECOND);
        gst_sample_unref(sample);
    }
    QVERIFY(isHiddenFile(files.reservation.audioSpoolPath));
    QVERIFY(created.contains(QStringLiteral("avenc_aac")));
    QVERIFY(!created.contains(QStringLiteral("mp4mux")));
    QVERIFY(observations.contains(QStringLiteral("avenc_aac:bitrate=192000")));
    QVERIFY(observations.contains(QStringLiteral("audio-appsrc:first-local-pts=0")));
    std::atomic_bool cancelled{false};
    const GstRecordingFinalizeResult result = pipeline.finalize(30'000, cancelled);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(nullPipelines.contains(QStringLiteral("video-spool")));
    QVERIFY(nullPipelines.contains(QStringLiteral("audio-spool")));
    QVERIFY(nullPipelines.contains(QStringLiteral("mp4-remux")));
    QCOMPARE(padEvents.count(QStringLiteral("video_%u:acquired")), 1);
    QCOMPARE(padEvents.count(QStringLiteral("video_%u:released")), 1);
    QCOMPARE(padEvents.count(QStringLiteral("audio_%u:acquired")), 1);
    QCOMPARE(padEvents.count(QStringLiteral("audio_%u:released")), 1);
    QVERIFY(isHiddenFile(files.reservation.audioSpoolPath));
    QVERIFY(isHiddenFile(files.reservation.temporaryMp4Path));
    const QString commitError = files.commit();
    QVERIFY2(commitError.isEmpty(), qPrintable(commitError));
    QVERIFY(!QFileInfo::exists(files.reservation.videoSpoolPath));
    QVERIFY(!QFileInfo::exists(files.reservation.audioSpoolPath));
    MediaCounts counts;
    QVERIFY2(discoverCounts(files.reservation.finalPath, &counts, &error), qPrintable(error));
    QCOMPARE(counts.video, 1);
    QCOMPARE(counts.audio, 1);
    GstClockTime videoStart = GST_CLOCK_TIME_NONE;
    GstClockTime audioStart = GST_CLOCK_TIME_NONE;
    QVERIFY2(firstDecodedRunningTimes(files.reservation.finalPath,
                                      &videoStart, &audioStart, &error), qPrintable(error));
    QVERIFY(std::llabs(static_cast<qint64>(videoStart)) <= 100'000'000);
    QVERIFY(std::llabs(static_cast<qint64>(audioStart) - origin) <= 100'000'000);
}

void GstRecordingPipelineTest::audioClaimFailureDoesNotOverwriteForeignFile()
{
    ReservedFiles files;
    QFile foreign(files.reservation.audioSpoolPath);
    QVERIFY(foreign.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(foreign.write("foreign-audio"), qint64(13));
    foreign.close();
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *video = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY(pipeline.start(configFor(files.reservation), video, &error));
    gst_sample_unref(video);
    GstSample *audio = makeAudioSample(4 * GST_SECOND);
    QVERIFY(!pipeline.pushAudio(audio, 1'200'000'000, &error));
    gst_sample_unref(audio);
    QVERIFY(error.contains(QStringLiteral("audio"), Qt::CaseInsensitive));
    QVERIFY(foreign.open(QIODevice::ReadOnly));
    QCOMPARE(foreign.readAll(), QByteArray("foreign-audio"));
    foreign.close();
    pipeline.abort();
    QVERIFY(QFileInfo::exists(files.reservation.audioSpoolPath));
    QFile::remove(files.reservation.audioSpoolPath);
}

void GstRecordingPipelineTest::blackFrameUsesLockedStrideAndDoesNotMutateBorrowedBuffers()
{
    ReservedFiles files;
    constexpr int stride = 640 * 4 + 16;
    bool sawBlack = false;
    bool blackZero = false;
    gsize blackSize = 0;
    GstClockTime blackPts = GST_CLOCK_TIME_NONE;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.bufferPushed = [&](const QString &track, GstBuffer *buffer) {
        if (track != QStringLiteral("black")) return;
        sawBlack = true;
        blackSize = gst_buffer_get_size(buffer);
        blackPts = GST_BUFFER_PTS(buffer);
        GstMapInfo map;
        blackZero = gst_buffer_map(buffer, &map, GST_MAP_READ);
        if (blackZero) {
            for (gsize index = 0; index < map.size; ++index) {
                if (map.data[index] != 0) {
                    blackZero = false;
                    break;
                }
            }
            gst_buffer_unmap(buffer, &map);
        }
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 5 * GST_SECOND, stride);
    QString error;
    QVERIFY2(pipeline.start(configFor(files.reservation), first, &error), qPrintable(error));
    QCOMPARE(GST_BUFFER_PTS(gst_sample_get_buffer(first)), 5 * GST_SECOND);
    gst_sample_unref(first);
    QVERIFY2(pipeline.pushBlackFrame(GST_SECOND, &error), qPrintable(error));
    QVERIFY(sawBlack);
    QVERIFY(blackZero);
    QCOMPARE(blackSize, static_cast<gsize>(stride) * 360);
    QCOMPARE(blackPts, GST_SECOND);

    GstSample *borrowed = makeVideoSample(640, 360, 9 * GST_SECOND, stride);
    GST_BUFFER_FLAG_SET(gst_sample_get_buffer(borrowed), GST_BUFFER_FLAG_DISCONT);
    QVERIFY(pipeline.pushVideo(borrowed, 2 * GST_SECOND, &error));
    QCOMPARE(GST_BUFFER_PTS(gst_sample_get_buffer(borrowed)), 9 * GST_SECOND);
    QVERIFY(GST_BUFFER_FLAG_IS_SET(gst_sample_get_buffer(borrowed), GST_BUFFER_FLAG_DISCONT));
    gst_sample_unref(borrowed);
    pipeline.abort();
}

void GstRecordingPipelineTest::invalidAudioAndDimensionChangeAreRejectedWithoutPush()
{
    ReservedFiles files;
    int pushed = 0;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.bufferPushed = [&](const QString &, GstBuffer *) { ++pushed; };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY(pipeline.start(configFor(files.reservation), first, &error));
    gst_sample_unref(first);
    const int afterFirst = pushed;
    GstSample *changed = makeVideoSample(800, 600, GST_SECOND);
    QVERIFY(!pipeline.pushVideo(changed, GST_SECOND, &error));
    gst_sample_unref(changed);
    QVERIFY(error.contains(QStringLiteral("dimensions changed"), Qt::CaseInsensitive));
    QCOMPARE(pushed, afterFirst);
    GstSample *invalidAudio = makeAudioSample(GST_SECOND, "F32LE");
    QVERIFY(!pipeline.pushAudio(invalidAudio, GST_SECOND, &error));
    gst_sample_unref(invalidAudio);
    QVERIFY(error.contains(QStringLiteral("S16LE")));
    QVERIFY(!QFileInfo::exists(files.reservation.audioSpoolPath));
    pipeline.abort();
}

void GstRecordingPipelineTest::injectedDeadlineAndCancellationAbortPromptly()
{
    {
        ReservedFiles files;
        qint64 now = 0;
        GstRecordingPipelineHooks hooks;
        hooks.encoderBuildAllowed = [](const QString &name, QString *) {
            return name == QStringLiteral("openh264enc");
        };
        hooks.monotonicMilliseconds = [&now] { const qint64 value = now; now += 5; return value; };
        hooks.busTimedPop = [](GstBus *, GstClockTime) -> GstMessage * { return nullptr; };
        GstRecordingPipeline pipeline(hooks);
        GstSample *first = makeVideoSample(640, 360, 0);
        QString error;
        QVERIFY(pipeline.start(configFor(files.reservation), first, &error));
        gst_sample_unref(first);
        std::atomic_bool cancelled{false};
        QElapsedTimer wall;
        wall.start();
        const GstRecordingFinalizeResult result = pipeline.finalize(10, cancelled);
        QVERIFY(!result.success);
        QVERIFY(result.error.contains(QStringLiteral("deadline"), Qt::CaseInsensitive));
        QVERIFY(wall.elapsed() < 500);
        QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    }
    {
        ReservedFiles files;
        GstRecordingPipelineHooks hooks;
        hooks.encoderBuildAllowed = [](const QString &name, QString *) {
            return name == QStringLiteral("openh264enc");
        };
        GstRecordingPipeline pipeline(hooks);
        GstSample *first = makeVideoSample(640, 360, 0);
        QString error;
        QVERIFY(pipeline.start(configFor(files.reservation), first, &error));
        gst_sample_unref(first);
        std::atomic_bool cancelled{true};
        const GstRecordingFinalizeResult result = pipeline.finalize(30'000, cancelled);
        QVERIFY(!result.success);
        QVERIFY(result.error.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));
        QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    }
}

void GstRecordingPipelineTest::injectedRemuxBusErrorIsSpecificAndReleasesPipeline()
{
    ReservedFiles files;
    int eosCount = 0;
    bool mp4HiddenDuringRemuxWait = false;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.busTimedPop = [&eosCount, &mp4HiddenDuringRemuxWait, &files]
        (GstBus *bus, GstClockTime timeout) -> GstMessage * {
        if (eosCount == 1) {
            mp4HiddenDuringRemuxWait = isHiddenFile(files.reservation.temporaryMp4Path);
            GError *error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
                                                "injected mux write failure");
            GstMessage *message = gst_message_new_error(GST_OBJECT(bus), error,
                                                        "injected remux debug");
            g_error_free(error);
            return message;
        }
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) ++eosCount;
        return message;
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *first = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY(pipeline.start(configFor(files.reservation), first, &error));
    gst_sample_unref(first);
    std::atomic_bool cancelled{false};
    const GstRecordingFinalizeResult result = pipeline.finalize(30'000, cancelled);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("MP4 remux")));
    QVERIFY(result.error.contains(QStringLiteral("injected mux write failure")));
    QVERIFY(mp4HiddenDuringRemuxWait);
    QVERIFY(isHiddenFile(files.reservation.temporaryMp4Path));
}

void GstRecordingPipelineTest::injectedTrackAndRemuxTimeouts_data()
{
    QTest::addColumn<QString>("targetLabel");
    QTest::newRow("audio-spool") << QStringLiteral("Audio spool");
    QTest::newRow("mp4-remux") << QStringLiteral("MP4 remux");
}

void GstRecordingPipelineTest::injectedTrackAndRemuxTimeouts()
{
    QFETCH(QString, targetLabel);
    ReservedFiles files;
    QString activeLabel;
    qint64 injectedNow = 0;
    QStringList nullPipelines;
    QStringList padEvents;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.pipelineStateObserved = [&nullPipelines](const QString &name, GstState state) {
        if (state == GST_STATE_NULL) nullPipelines.append(name);
    };
    hooks.requestPadObserved = [&padEvents](const QString &name, bool acquired) {
        padEvents.append(name + (acquired ? QStringLiteral(":acquired")
                                          : QStringLiteral(":released")));
    };
    hooks.monotonicMilliseconds = [&] {
        return activeLabel == targetLabel ? injectedNow++ : qint64(0);
    };
    hooks.labeledBusTimedPop = [&](const QString &label, GstBus *bus,
                                    GstClockTime timeout) -> GstMessage * {
        activeLabel = label;
        if (label == targetLabel) return nullptr;
        return gst_bus_timed_pop_filtered(
            bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *video = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY(pipeline.start(configFor(files.reservation), video, &error));
    gst_sample_unref(video);
    GstSample *audio = makeAudioSample(4 * GST_SECOND);
    QVERIFY(pipeline.pushAudio(audio, 1'200'000'000, &error));
    gst_sample_unref(audio);
    std::atomic_bool cancelled{false};
    const GstRecordingFinalizeResult result = pipeline.finalize(10, cancelled);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(targetLabel));
    QVERIFY(result.error.contains(QStringLiteral("deadline"), Qt::CaseInsensitive));
    QVERIFY(nullPipelines.contains(QStringLiteral("video-spool")));
    QVERIFY(nullPipelines.contains(QStringLiteral("audio-spool")));
    if (targetLabel == QStringLiteral("MP4 remux")) {
        QVERIFY(nullPipelines.contains(QStringLiteral("mp4-remux")));
        QCOMPARE(padEvents.count(QStringLiteral("video_%u:acquired")), 1);
        QCOMPARE(padEvents.count(QStringLiteral("video_%u:released")), 1);
        QCOMPARE(padEvents.count(QStringLiteral("audio_%u:acquired")), 1);
        QCOMPARE(padEvents.count(QStringLiteral("audio_%u:released")), 1);
    } else {
        QVERIFY(padEvents.isEmpty());
    }
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    QVERIFY(isHiddenFile(files.reservation.audioSpoolPath));
    QVERIFY(isHiddenFile(files.reservation.temporaryMp4Path));
}

void GstRecordingPipelineTest::injectedPipelineBusErrorsAreClassified_data()
{
    QTest::addColumn<QString>("targetLabel");
    QTest::addColumn<QString>("injectedText");
    QTest::addColumn<QString>("classification");
    QTest::addColumn<bool>("withAudio");
    QTest::newRow("video-encode") << QStringLiteral("Video spool")
        << QStringLiteral("injected H.264 encode failure") << QStringLiteral("encode") << false;
    QTest::newRow("video-write") << QStringLiteral("Video spool")
        << QStringLiteral("injected video write failure") << QStringLiteral("write") << false;
    QTest::newRow("audio-encode") << QStringLiteral("Audio spool")
        << QStringLiteral("injected AAC encode failure") << QStringLiteral("encode") << true;
    QTest::newRow("remux-demux") << QStringLiteral("MP4 remux")
        << QStringLiteral("injected demux failure") << QStringLiteral("demux") << false;
    QTest::newRow("remux-mux") << QStringLiteral("MP4 remux")
        << QStringLiteral("injected mux failure") << QStringLiteral("mux") << false;
}

void GstRecordingPipelineTest::injectedPipelineBusErrorsAreClassified()
{
    QFETCH(QString, targetLabel);
    QFETCH(QString, injectedText);
    QFETCH(QString, classification);
    QFETCH(bool, withAudio);
    ReservedFiles files;
    QStringList nullPipelines;
    QStringList padEvents;
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    hooks.pipelineStateObserved = [&nullPipelines](const QString &name, GstState state) {
        if (state == GST_STATE_NULL) nullPipelines.append(name);
    };
    hooks.requestPadObserved = [&padEvents](const QString &name, bool acquired) {
        padEvents.append(name + (acquired ? QStringLiteral(":acquired")
                                          : QStringLiteral(":released")));
    };
    hooks.labeledBusTimedPop = [&](const QString &label, GstBus *bus,
                                    GstClockTime timeout) -> GstMessage * {
        if (label == targetLabel) return injectedErrorMessage(bus, injectedText);
        return gst_bus_timed_pop_filtered(
            bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    };
    GstRecordingPipeline pipeline(hooks);
    GstSample *video = makeVideoSample(640, 360, 0);
    QString error;
    QVERIFY(pipeline.start(configFor(files.reservation), video, &error));
    gst_sample_unref(video);
    if (withAudio) {
        GstSample *audio = makeAudioSample(4 * GST_SECOND);
        QVERIFY(pipeline.pushAudio(audio, 1'200'000'000, &error));
        gst_sample_unref(audio);
    }
    std::atomic_bool cancelled{false};
    const GstRecordingFinalizeResult result = pipeline.finalize(30'000, cancelled);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(targetLabel));
    QVERIFY(result.error.contains(classification, Qt::CaseInsensitive));
    QVERIFY(nullPipelines.contains(QStringLiteral("video-spool")));
    if (withAudio) QVERIFY(nullPipelines.contains(QStringLiteral("audio-spool")));
    if (targetLabel == QStringLiteral("MP4 remux")) {
        QVERIFY(nullPipelines.contains(QStringLiteral("mp4-remux")));
        QCOMPARE(padEvents.count(QStringLiteral("video_%u:acquired")), 1);
        QCOMPARE(padEvents.count(QStringLiteral("video_%u:released")), 1);
    } else {
        QVERIFY(padEvents.isEmpty());
    }
    QVERIFY(isHiddenFile(files.reservation.videoSpoolPath));
    QVERIFY(isHiddenFile(files.reservation.temporaryMp4Path));
}

static_assert(!std::is_copy_constructible_v<GstRecordingPipeline>);
static_assert(!std::is_move_constructible_v<GstRecordingPipeline>);

QTEST_APPLESS_MAIN(GstRecordingPipelineTest)

#include "GstRecordingPipelineTest.moc"

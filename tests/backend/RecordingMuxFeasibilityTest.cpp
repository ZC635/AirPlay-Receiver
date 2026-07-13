#include <QtTest/QtTest>

#include <QTemporaryDir>
#include <QUrl>

#include <array>
#include <cmath>
#include <future>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/pbutils/pbutils.h>

namespace {

constexpr GstClockTime kSecond = GST_SECOND;

class PipelineGuard {
public:
    explicit PipelineGuard(GstElement *pipeline = nullptr) : m_pipeline(pipeline) {}
    ~PipelineGuard() {
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            gst_object_unref(m_pipeline);
        }
    }

private:
    GstElement *m_pipeline;
};

struct TrackSpool {
    QString path;
    GstClockTime originPts = 0;
};

struct FirstDecodedPts {
    GstClockTime video = GST_CLOCK_TIME_NONE;
    GstClockTime audio = GST_CLOCK_TIME_NONE;
};

bool writeVideoSpool(const TrackSpool &, QString *);
bool writeAudioSpool(const TrackSpool &, QString *);
bool remuxSpools(const TrackSpool &, const TrackSpool *, const QString &, QString *);
bool readFirstDecodedPts(const QString &, bool, FirstDecodedPts *, QString *);

QString busError(GstMessage *message) {
    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const QString result = QStringLiteral("%1: %2; debug=%3")
                               .arg(QString::fromUtf8(GST_OBJECT_NAME(message->src)))
                               .arg(QString::fromUtf8(error ? error->message : "unknown"))
                               .arg(QString::fromUtf8(debug ? debug : "none"));
    g_clear_error(&error);
    g_free(debug);
    return result;
}

bool waitForEos(GstElement *pipeline, GstClockTime timeout, QString *errorText) {
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *terminal = gst_bus_timed_pop_filtered(
        bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    gst_object_unref(bus);
    if (!terminal) {
        *errorText = QStringLiteral("deadline waiting for EOS");
        return false;
    }
    const bool succeeded = GST_MESSAGE_TYPE(terminal) == GST_MESSAGE_EOS;
    if (!succeeded) *errorText = busError(terminal);
    gst_message_unref(terminal);
    return succeeded;
}

bool writeVideoSpool(const TrackSpool &spool, QString *errorText) {
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=90 is-live=false ! "
        "video/x-raw,width=640,height=360,framerate=30/1 ! videoconvert ! "
        "openh264enc bitrate=4000000 rate-control=bitrate ! "
        "h264parse ! video/x-h264,stream-format=avc,alignment=au ! "
        "matroskamux offset-to-zero=true ! filesink name=file_sink",
        &error);
    PipelineGuard cleanup(pipeline);
    if (error || !pipeline) {
        *errorText = QString::fromUtf8(error ? error->message : "video spool pipeline creation failed");
        g_clear_error(&error);
        return false;
    }

    GstElement *fileSink = gst_bin_get_by_name(GST_BIN(pipeline), "file_sink");
    if (!fileSink) {
        *errorText = QStringLiteral("video spool filesink missing");
        return false;
    }
    g_object_set(fileSink, "location", spool.path.toUtf8().constData(), nullptr);
    gst_object_unref(fileSink);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *errorText = QStringLiteral("video spool pipeline failed to enter PLAYING");
        return false;
    }
    return waitForEos(pipeline, 15 * GST_SECOND, errorText);
}

bool writeAudioSpool(const TrackSpool &spool, QString *errorText) {
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=audio_source is-live=false format=time "
        "caps=audio/x-raw,format=S16LE,rate=44100,channels=2,layout=interleaved ! "
        "audioconvert ! audioresample ! "
        "audio/x-raw,rate=44100,channels=2,layout=interleaved ! "
        "avenc_aac bitrate=192000 ! aacparse ! "
        "audio/mpeg,mpegversion=4,stream-format=raw ! "
        "matroskamux offset-to-zero=true ! filesink name=file_sink",
        &error);
    PipelineGuard cleanup(pipeline);
    if (error || !pipeline) {
        *errorText = QString::fromUtf8(error ? error->message : "audio spool pipeline creation failed");
        g_clear_error(&error);
        return false;
    }

    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "audio_source");
    GstElement *fileSink = gst_bin_get_by_name(GST_BIN(pipeline), "file_sink");
    if (!source || !fileSink) {
        *errorText = QStringLiteral("audio spool appsrc or filesink missing");
        if (source) gst_object_unref(source);
        if (fileSink) gst_object_unref(fileSink);
        return false;
    }
    g_object_set(fileSink, "location", spool.path.toUtf8().constData(), nullptr);
    gst_object_unref(fileSink);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *errorText = QStringLiteral("audio spool pipeline failed to enter PLAYING");
        gst_object_unref(source);
        return false;
    }

    constexpr guint framesPerBuffer = 441;
    constexpr guint bufferCount = 200;
    constexpr GstClockTime bufferDuration = 10 * GST_MSECOND;
    std::array<gint16, framesPerBuffer * 2> samples{};
    for (guint bufferIndex = 0; bufferIndex < bufferCount; ++bufferIndex) {
        for (guint frame = 0; frame < framesPerBuffer; ++frame) {
            const double time = static_cast<double>(bufferIndex * framesPerBuffer + frame) / 44100.0;
            const gint16 value = static_cast<gint16>(12000.0 * std::sin(2.0 * M_PI * 440.0 * time));
            samples[frame * 2] = value;
            samples[frame * 2 + 1] = value;
        }
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, sizeof(samples), nullptr);
        gst_buffer_fill(buffer, 0, samples.data(), sizeof(samples));
        GST_BUFFER_PTS(buffer) = bufferIndex * bufferDuration;
        GST_BUFFER_DURATION(buffer) = bufferDuration;
        const GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
        if (result != GST_FLOW_OK) {
            *errorText = QStringLiteral("audio spool push failed: %1").arg(result);
            gst_object_unref(source);
            return false;
        }
    }
    const GstFlowReturn eosResult = gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    if (eosResult != GST_FLOW_OK) {
        *errorText = QStringLiteral("audio spool EOS failed: %1").arg(eosResult);
        return false;
    }
    return waitForEos(pipeline, 15 * GST_SECOND, errorText);
}

struct DemuxLinkTarget {
    GstElement *queue = nullptr;
    const char *capsName = nullptr;
};

void linkDemuxPad(GstElement *, GstPad *pad, gpointer userData) {
    auto *target = static_cast<DemuxLinkTarget *>(userData);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const GstStructure *structure = caps && !gst_caps_is_empty(caps)
        ? gst_caps_get_structure(caps, 0)
        : nullptr;
    const char *name = structure ? gst_structure_get_name(structure) : nullptr;
    if (g_strcmp0(name, target->capsName) == 0) {
        GstPad *sinkPad = gst_element_get_static_pad(target->queue, "sink");
        if (!gst_pad_is_linked(sinkPad)) gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
    }
    if (caps) gst_caps_unref(caps);
}

class RequestPadGuard {
public:
    RequestPadGuard(GstElement *element, GstPad *pad) : m_element(element), m_pad(pad) {}
    ~RequestPadGuard() {
        if (m_pad) {
            gst_element_release_request_pad(m_element, m_pad);
            gst_object_unref(m_pad);
        }
    }
private:
    GstElement *m_element;
    GstPad *m_pad;
};

bool remuxSpools(const TrackSpool &video, const TrackSpool *audio,
                 const QString &mp4Path, QString *errorText) {
    GstElement *pipeline = gst_pipeline_new("spool_remux");
    PipelineGuard cleanup(pipeline);
    GstElement *videoSource = gst_element_factory_make("filesrc", nullptr);
    GstElement *videoDemux = gst_element_factory_make("matroskademux", nullptr);
    GstElement *videoQueue = gst_element_factory_make("queue", nullptr);
    GstElement *videoParse = gst_element_factory_make("h264parse", nullptr);
    GstElement *videoOffset = gst_element_factory_make("identity", nullptr);
    GstElement *videoCaps = gst_element_factory_make("capsfilter", nullptr);
    GstElement *mux = gst_element_factory_make("mp4mux", nullptr);
    GstElement *sink = gst_element_factory_make("filesink", nullptr);
    if (!pipeline || !videoSource || !videoDemux || !videoQueue || !videoParse ||
        !videoOffset || !videoCaps || !mux || !sink) {
        *errorText = QStringLiteral("required video remux element missing");
        return false;
    }

    g_object_set(videoSource, "location", video.path.toUtf8().constData(), nullptr);
    g_object_set(videoOffset, "ts-offset", static_cast<gint64>(video.originPts), nullptr);
    GstPad *videoOffsetPad = gst_element_get_static_pad(videoOffset, "src");
    gst_pad_set_offset(videoOffsetPad, static_cast<gint64>(video.originPts));
    gst_object_unref(videoOffsetPad);
    g_object_set(sink, "location", mp4Path.toUtf8().constData(), nullptr);
    GstCaps *h264Caps = gst_caps_from_string("video/x-h264,stream-format=avc,alignment=au");
    g_object_set(videoCaps, "caps", h264Caps, nullptr);
    gst_caps_unref(h264Caps);
    gst_bin_add_many(GST_BIN(pipeline), videoSource, videoDemux, videoQueue, videoParse,
                     videoOffset, videoCaps, mux, sink, nullptr);
    if (!gst_element_link(videoSource, videoDemux) ||
        !gst_element_link_many(videoQueue, videoParse, videoOffset, videoCaps, nullptr) ||
        !gst_element_link(mux, sink)) {
        *errorText = QStringLiteral("failed to link video remux elements");
        return false;
    }
    DemuxLinkTarget videoTarget{videoQueue, "video/x-h264"};
    g_signal_connect(videoDemux, "pad-added", G_CALLBACK(linkDemuxPad), &videoTarget);

    GstPad *videoMuxPad = gst_element_request_pad_simple(mux, "video_%u");
    RequestPadGuard videoPadCleanup(mux, videoMuxPad);
    GstPad *videoSrcPad = gst_element_get_static_pad(videoCaps, "src");
    const GstPadLinkReturn videoLink = videoMuxPad && videoSrcPad
        ? gst_pad_link(videoSrcPad, videoMuxPad)
        : GST_PAD_LINK_REFUSED;
    if (videoSrcPad) gst_object_unref(videoSrcPad);
    if (!videoMuxPad || videoLink != GST_PAD_LINK_OK) {
        *errorText = QStringLiteral("failed to request/link mp4mux video_%u pad: %1")
                         .arg(static_cast<int>(videoLink));
        return false;
    }

    GstElement *audioSource = nullptr;
    GstElement *audioDemux = nullptr;
    GstElement *audioQueue = nullptr;
    GstElement *audioParse = nullptr;
    GstElement *audioOffset = nullptr;
    GstElement *audioCaps = nullptr;
    GstPad *audioMuxPad = nullptr;
    DemuxLinkTarget audioTarget;
    if (audio) {
        audioSource = gst_element_factory_make("filesrc", nullptr);
        audioDemux = gst_element_factory_make("matroskademux", nullptr);
        audioQueue = gst_element_factory_make("queue", nullptr);
        audioParse = gst_element_factory_make("aacparse", nullptr);
        audioOffset = gst_element_factory_make("identity", nullptr);
        audioCaps = gst_element_factory_make("capsfilter", nullptr);
        if (!audioSource || !audioDemux || !audioQueue || !audioParse || !audioOffset || !audioCaps) {
            *errorText = QStringLiteral("required audio remux element missing");
            return false;
        }
        g_object_set(audioSource, "location", audio->path.toUtf8().constData(), nullptr);
        g_object_set(audioOffset, "ts-offset", static_cast<gint64>(audio->originPts), nullptr);
        GstPad *audioOffsetPad = gst_element_get_static_pad(audioOffset, "src");
        gst_pad_set_offset(audioOffsetPad, static_cast<gint64>(audio->originPts));
        gst_object_unref(audioOffsetPad);
        GstCaps *aacCaps = gst_caps_from_string("audio/mpeg,mpegversion=4,stream-format=raw");
        g_object_set(audioCaps, "caps", aacCaps, nullptr);
        gst_caps_unref(aacCaps);
        gst_bin_add_many(GST_BIN(pipeline), audioSource, audioDemux, audioQueue, audioParse,
                         audioOffset, audioCaps, nullptr);
        if (!gst_element_link(audioSource, audioDemux) ||
            !gst_element_link_many(audioQueue, audioParse, audioOffset, audioCaps, nullptr)) {
            *errorText = QStringLiteral("failed to link audio remux elements");
            return false;
        }
        audioTarget = {audioQueue, "audio/mpeg"};
        g_signal_connect(audioDemux, "pad-added", G_CALLBACK(linkDemuxPad), &audioTarget);
        audioMuxPad = gst_element_request_pad_simple(mux, "audio_%u");
    }
    RequestPadGuard audioPadCleanup(mux, audioMuxPad);
    if (audio) {
        GstPad *audioSrcPad = gst_element_get_static_pad(audioCaps, "src");
        const GstPadLinkReturn audioLink = audioMuxPad && audioSrcPad
            ? gst_pad_link(audioSrcPad, audioMuxPad)
            : GST_PAD_LINK_REFUSED;
        if (audioSrcPad) gst_object_unref(audioSrcPad);
        if (!audioMuxPad || audioLink != GST_PAD_LINK_OK) {
            *errorText = QStringLiteral("failed to request/link mp4mux audio_%u pad: %1")
                             .arg(static_cast<int>(audioLink));
            return false;
        }
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *errorText = QStringLiteral("remux pipeline failed to enter PLAYING");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        return false;
    }
    const bool succeeded = waitForEos(pipeline, 15 * GST_SECOND, errorText);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    return succeeded;
}

struct DecodeLinkTargets {
    GstElement *videoSink = nullptr;
    GstElement *audioSink = nullptr;
};

void linkDecodedPad(GstElement *, GstPad *pad, gpointer userData) {
    auto *targets = static_cast<DecodeLinkTargets *>(userData);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const GstStructure *structure = caps && !gst_caps_is_empty(caps)
        ? gst_caps_get_structure(caps, 0)
        : nullptr;
    const char *name = structure ? gst_structure_get_name(structure) : nullptr;
    GstElement *sink = g_strcmp0(name, "video/x-raw") == 0
        ? targets->videoSink
        : (g_strcmp0(name, "audio/x-raw") == 0 ? targets->audioSink : nullptr);
    if (sink) {
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        if (!gst_pad_is_linked(sinkPad)) gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
    }
    if (caps) gst_caps_unref(caps);
}

bool pullFirstPts(GstElement *sink, const char *track, GstClockTime *pts, QString *errorText) {
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
    if (!sample) {
        *errorText = QStringLiteral("deadline waiting for first decoded %1 sample")
                         .arg(QString::fromUtf8(track));
        return false;
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    const GstClockTime value = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
    const GstSegment *segment = gst_sample_get_segment(sample);
    const GstClockTime runningTime = segment && GST_CLOCK_TIME_IS_VALID(value)
        ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, value)
        : GST_CLOCK_TIME_NONE;
    gst_sample_unref(sample);
    if (!GST_CLOCK_TIME_IS_VALID(value) || !GST_CLOCK_TIME_IS_VALID(runningTime)) {
        *errorText = QStringLiteral("first decoded %1 sample has invalid PTS or running time")
                         .arg(QString::fromUtf8(track));
        return false;
    }
    *pts = runningTime;
    return true;
}

bool readFirstDecodedPts(const QString &path, bool expectAudio,
                         FirstDecodedPts *pts, QString *errorText) {
    pts->video = GST_CLOCK_TIME_NONE;
    pts->audio = GST_CLOCK_TIME_NONE;
    GstElement *pipeline = gst_pipeline_new("decode_first_pts");
    PipelineGuard cleanup(pipeline);
    GstElement *decode = gst_element_factory_make("uridecodebin", nullptr);
    GstElement *videoSink = gst_element_factory_make("appsink", nullptr);
    GstElement *audioSink = expectAudio ? gst_element_factory_make("appsink", nullptr) : nullptr;
    if (!pipeline || !decode || !videoSink || (expectAudio && !audioSink)) {
        *errorText = QStringLiteral("required decoded-PTS element missing");
        return false;
    }
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    g_object_set(decode, "uri", uri.constData(), nullptr);
    g_object_set(videoSink, "sync", FALSE, "max-buffers", 1u, "drop", TRUE, nullptr);
    if (audioSink) {
        g_object_set(audioSink, "sync", FALSE, "max-buffers", 1u, "drop", TRUE, nullptr);
        gst_bin_add_many(GST_BIN(pipeline), decode, videoSink, audioSink, nullptr);
    } else {
        gst_bin_add_many(GST_BIN(pipeline), decode, videoSink, nullptr);
    }
    DecodeLinkTargets targets{videoSink, audioSink};
    g_signal_connect(decode, "pad-added", G_CALLBACK(linkDecodedPad), &targets);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *errorText = QStringLiteral("decoded-PTS pipeline failed to enter PLAYING");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        return false;
    }
    if (!audioSink) {
        const bool succeeded = pullFirstPts(videoSink, "video", &pts->video, errorText);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        return succeeded;
    }

    QString videoError;
    QString audioError;
    auto videoPull = std::async(std::launch::async, [&] {
        return pullFirstPts(videoSink, "video", &pts->video, &videoError);
    });
    auto audioPull = std::async(std::launch::async, [&] {
        return pullFirstPts(audioSink, "audio", &pts->audio, &audioError);
    });
    const bool videoSucceeded = videoPull.get();
    const bool audioSucceeded = audioPull.get();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (!videoSucceeded || !audioSucceeded) {
        *errorText = !videoSucceeded ? videoError : audioError;
        return false;
    }
    return true;
}

struct MediaFacts {
    int videoCount = 0;
    int audioCount = 0;
    bool h264 = false;
    bool aac = false;
    int width = 0;
    int height = 0;
    int fpsNumerator = 0;
    int fpsDenominator = 1;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
};

bool discoverMedia(const QString &path, MediaFacts *facts, QString *errorText) {
    GError *error = nullptr;
    GstDiscoverer *discoverer = gst_discoverer_new(5 * GST_SECOND, &error);
    if (!discoverer) {
        *errorText = QString::fromUtf8(error ? error->message : "discoverer creation failed");
        g_clear_error(&error);
        return false;
    }
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri.constData(), &error);
    if (!info || error || gst_discoverer_info_get_result(info) != GST_DISCOVERER_OK) {
        *errorText = QString::fromUtf8(error ? error->message : "discoverer did not return OK");
        g_clear_error(&error);
        if (info) gst_discoverer_info_unref(info);
        g_object_unref(discoverer);
        return false;
    }

    facts->duration = gst_discoverer_info_get_duration(info);
    GList *streams = gst_discoverer_info_get_stream_list(info);
    for (GList *node = streams; node; node = node->next) {
        auto *stream = GST_DISCOVERER_STREAM_INFO(node->data);
        GstCaps *caps = gst_discoverer_stream_info_get_caps(stream);
        const GstStructure *structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        const gchar *name = structure ? gst_structure_get_name(structure) : "";
        if (GST_IS_DISCOVERER_VIDEO_INFO(stream)) {
            auto *video = GST_DISCOVERER_VIDEO_INFO(stream);
            facts->videoCount++;
            facts->h264 |= g_strcmp0(name, "video/x-h264") == 0;
            facts->width = gst_discoverer_video_info_get_width(video);
            facts->height = gst_discoverer_video_info_get_height(video);
            facts->fpsNumerator = gst_discoverer_video_info_get_framerate_num(video);
            facts->fpsDenominator = gst_discoverer_video_info_get_framerate_denom(video);
        } else if (GST_IS_DISCOVERER_AUDIO_INFO(stream)) {
            facts->audioCount++;
            facts->aac |= g_strcmp0(name, "audio/mpeg") == 0;
        }
        if (caps) gst_caps_unref(caps);
    }
    gst_discoverer_stream_info_list_free(streams);
    gst_discoverer_info_unref(info);
    g_object_unref(discoverer);
    return true;
}

void verifyVideoFacts(const MediaFacts &facts) {
    QCOMPARE(facts.videoCount, 1);
    QVERIFY(facts.h264);
    QCOMPARE(facts.width, 640);
    QCOMPARE(facts.height, 360);
    QCOMPARE(facts.fpsNumerator, 30);
    QCOMPARE(facts.fpsDenominator, 1);
    QVERIFY(std::abs(static_cast<qint64>(facts.duration) - static_cast<qint64>(3 * kSecond)) <=
            static_cast<qint64>(150 * GST_MSECOND));
}

} // namespace

class RecordingMuxFeasibilityTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }

    void videoOnlySpoolRemuxContainsNoAudioTrack() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const TrackSpool video{directory.filePath(QStringLiteral("video.mkv")), 0};
        const QString path = directory.filePath(QStringLiteral("video-only.mp4"));
        QString error;
        QVERIFY2(writeVideoSpool(video, &error), qPrintable(error));
        QVERIFY2(remuxSpools(video, nullptr, path, &error), qPrintable(error));

        MediaFacts facts;
        QVERIFY2(discoverMedia(path, &facts, &error), qPrintable(error));
        verifyVideoFacts(facts);
        QCOMPARE(facts.audioCount, 0);

        FirstDecodedPts pts;
        QVERIFY2(readFirstDecodedPts(path, false, &pts, &error), qPrintable(error));
        QVERIFY(std::abs(static_cast<qint64>(pts.video)) <= static_cast<qint64>(100 * GST_MSECOND));
        QCOMPARE(pts.audio, GST_CLOCK_TIME_NONE);
    }

    void lateAudioSpoolRemuxStartsAudioAtOneSecond() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const TrackSpool video{directory.filePath(QStringLiteral("video.mkv")), 0};
        const TrackSpool audio{directory.filePath(QStringLiteral("audio.mka")), GST_SECOND};
        const QString path = directory.filePath(QStringLiteral("late-audio.mp4"));
        QString error;
        QVERIFY2(writeVideoSpool(video, &error), qPrintable(error));
        QVERIFY2(writeAudioSpool(audio, &error), qPrintable(error));
        QVERIFY2(remuxSpools(video, &audio, path, &error), qPrintable(error));

        MediaFacts facts;
        QVERIFY2(discoverMedia(path, &facts, &error), qPrintable(error));
        verifyVideoFacts(facts);
        QCOMPARE(facts.audioCount, 1);
        QVERIFY(facts.aac);

        FirstDecodedPts pts;
        QVERIFY2(readFirstDecodedPts(path, true, &pts, &error), qPrintable(error));
        QVERIFY(std::abs(static_cast<qint64>(pts.video)) <= static_cast<qint64>(100 * GST_MSECOND));
        QVERIFY2(std::abs(static_cast<qint64>(pts.audio) - static_cast<qint64>(GST_SECOND)) <=
                     static_cast<qint64>(100 * GST_MSECOND),
                 qPrintable(QStringLiteral("first decoded audio running time was %1 ns").arg(pts.audio)));
    }
};

QTEST_GUILESS_MAIN(RecordingMuxFeasibilityTest)
#include "RecordingMuxFeasibilityTest.moc"

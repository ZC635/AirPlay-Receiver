#include "backend/GstRecordingPipeline.h"
#include "backend/RecordingController.h"
#include "backend/RecordingFileTransaction.h"
#include "backend/RecordingTimeline.h"

#include <QtTest>

#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <atomic>
#include <cmath>
#include <future>

namespace {

struct StreamSummary {
    QString codec;
    int bitrate = 0;
    int taggedBitrate = 0;
    int width = 0;
    int height = 0;
    int fpsNumerator = 0;
    int fpsDenominator = 1;
    int parNumerator = 1;
    int parDenominator = 1;
};

struct MediaSummary {
    QString container;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
    int reportedBitrate = 0;
    QList<StreamSummary> video;
    QList<StreamSummary> audio;
};

QString capsName(GstCaps *caps)
{
    if (!caps || gst_caps_is_empty(caps)) return QStringLiteral("<none>");
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    return structure ? QString::fromUtf8(gst_structure_get_name(structure))
                     : QStringLiteral("<none>");
}

bool discover(const QString &path, MediaSummary *summary, QString *error)
{
    const QString uriText = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    GError *createError = nullptr;
    GstDiscoverer *discoverer = gst_discoverer_new(20 * GST_SECOND, &createError);
    if (!discoverer) {
        *error = QStringLiteral("URI=%1; GError=%2").arg(
            uriText, QString::fromUtf8(createError ? createError->message : "discoverer creation failed"));
        g_clear_error(&createError);
        return false;
    }
    GError *discoverError = nullptr;
    GstDiscovererInfo *info = gst_discoverer_discover_uri(
        discoverer, uriText.toUtf8().constData(), &discoverError);
    if (!info || discoverError) {
        *error = QStringLiteral("URI=%1; GError=%2").arg(
            uriText, QString::fromUtf8(discoverError ? discoverError->message : "discovery failed"));
        g_clear_error(&discoverError);
        if (info) gst_discoverer_info_unref(info);
        g_object_unref(discoverer);
        return false;
    }

    summary->duration = gst_discoverer_info_get_duration(info);
    const GstTagList *tags = gst_discoverer_info_get_tags(info);
    guint reportedBitrate = 0;
    if (tags && gst_tag_list_get_uint(tags, GST_TAG_BITRATE, &reportedBitrate)) {
        summary->reportedBitrate = static_cast<int>(reportedBitrate);
    }
    GstDiscovererStreamInfo *root = gst_discoverer_info_get_stream_info(info);
    if (root) {
        GstCaps *caps = gst_discoverer_stream_info_get_caps(root);
        summary->container = capsName(caps);
        if (caps) gst_caps_unref(caps);
        gst_discoverer_stream_info_unref(root);
    }
    GList *streams = gst_discoverer_info_get_stream_list(info);
    for (GList *entry = streams; entry; entry = entry->next) {
        auto *stream = GST_DISCOVERER_STREAM_INFO(entry->data);
        GstCaps *caps = gst_discoverer_stream_info_get_caps(stream);
        StreamSummary details;
        details.codec = capsName(caps);
        const GstTagList *streamTags = gst_discoverer_stream_info_get_tags(stream);
        guint taggedBitrate = 0;
        if (streamTags && gst_tag_list_get_uint(streamTags, GST_TAG_BITRATE, &taggedBitrate)) {
            details.taggedBitrate = static_cast<int>(taggedBitrate);
        }
        if (GST_IS_DISCOVERER_VIDEO_INFO(stream)) {
            auto *video = GST_DISCOVERER_VIDEO_INFO(stream);
            details.bitrate = gst_discoverer_video_info_get_bitrate(video);
            details.width = gst_discoverer_video_info_get_width(video);
            details.height = gst_discoverer_video_info_get_height(video);
            details.fpsNumerator = gst_discoverer_video_info_get_framerate_num(video);
            details.fpsDenominator = gst_discoverer_video_info_get_framerate_denom(video);
            details.parNumerator = gst_discoverer_video_info_get_par_num(video);
            details.parDenominator = gst_discoverer_video_info_get_par_denom(video);
            if (caps && gst_caps_get_size(caps) > 0) {
                const GstStructure *structure = gst_caps_get_structure(caps, 0);
                (void)gst_structure_get_fraction(structure, "pixel-aspect-ratio",
                                                 &details.parNumerator, &details.parDenominator);
            }
            summary->video.append(details);
        } else if (GST_IS_DISCOVERER_AUDIO_INFO(stream)) {
            details.bitrate = gst_discoverer_audio_info_get_bitrate(
                GST_DISCOVERER_AUDIO_INFO(stream));
            summary->audio.append(details);
        }
        if (caps) gst_caps_unref(caps);
    }
    gst_discoverer_stream_info_list_free(streams);
    gst_discoverer_info_unref(info);
    g_object_unref(discoverer);
    return true;
}

int reportedVideoBitrate(const MediaSummary &media)
{
    for (const StreamSummary &stream : media.video) {
        if (stream.bitrate > 0) return stream.bitrate;
        if (stream.taggedBitrate > 0) return stream.taggedBitrate;
    }
    return media.reportedBitrate;
}

bool reportedBitrateInTier(int reported, int targetBitsPerSecond,
                           int lowerPercent = 35, int upperPercent = 135)
{
    // GstDiscoverer and GST_TAG_BITRATE report bits per second.
    if (reported <= 0) return false;
    const qint64 lower = targetBitsPerSecond * qint64(lowerPercent) / 100;
    const qint64 upper = targetBitsPerSecond * qint64(upperPercent) / 100;
    return reported >= lower && reported <= upper;
}

QString bitrateDiagnostic(const MediaSummary &media)
{
    const StreamSummary stream = media.video.isEmpty() ? StreamSummary{} : media.video.first();
    return QStringLiteral("container=%1; video=%2; video-tag=%3; selected=%4")
        .arg(media.reportedBitrate).arg(stream.bitrate).arg(stream.taggedBitrate)
        .arg(reportedVideoBitrate(media));
}

GstSample *videoSample(int width, int height, int fps, GstClockTime pts,
                       const QByteArray &pixels = {})
{
    const gsize size = static_cast<gsize>(width) * static_cast<gsize>(height) * 4;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / fps;
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        if (pixels.size() == static_cast<int>(size)) {
            memcpy(map.data, pixels.constData(), size);
        } else {
            memset(map.data, 0x30, size);
        }
        gst_buffer_unmap(buffer, &map);
    }
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA",
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, fps, 1,
        "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

GstSample *busyVideoSample(int width, int height, int fps, GstClockTime pts, quint32 seed,
                           int blockPixels = 4)
{
    QByteArray pixels(width * height * 4, char(0));
    quint32 state = seed;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x % blockPixels == 0 && y % blockPixels == 0)
                state = state * 1664525u + 1013904223u;
            const int offset = (y * width + x) * 4;
            pixels[offset] = char(state & 0xffu);
            pixels[offset + 1] = char((state >> 8) & 0xffu);
            pixels[offset + 2] = char((state >> 16) & 0xffu);
            pixels[offset + 3] = char(255);
        }
    }
    return videoSample(width, height, fps, pts, pixels);
}

GstSample *audioSample(GstClockTime pts)
{
    constexpr int samples = 441;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, samples * 2 * sizeof(gint16), nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        auto *pcm = reinterpret_cast<gint16 *>(map.data);
        quint32 state = static_cast<quint32>(pts / GST_MSECOND) ^ 0xaac19200u;
        for (int index = 0; index < samples * 2; ++index) {
            state = state * 1664525u + 1013904223u;
            pcm[index] = static_cast<gint16>(state >> 16);
        }
        gst_buffer_unmap(buffer, &map);
    }
    GstCaps *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
        "rate", G_TYPE_INT, 44100, "channels", G_TYPE_INT, 2,
        "layout", G_TYPE_STRING, "interleaved", nullptr);
    GstSample *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

struct RgbComparison {
    int mismatches = 0;
    int total = 0;
};

RgbComparison compareRgbPixels(const QByteArray &expected, const quint8 *actual,
                               int width, int height, gsize actualRowStride,
                               int channelTolerance)
{
    RgbComparison result;
    const auto *expectedPixels = reinterpret_cast<const quint8 *>(expected.constData());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const quint8 *want = expectedPixels + (y * width + x) * 4;
            const quint8 *got = actual + y * actualRowStride + x * 4;
            ++result.total;
            if (std::abs(int(want[0]) - int(got[0])) > channelTolerance ||
                std::abs(int(want[1]) - int(got[1])) > channelTolerance ||
                std::abs(int(want[2]) - int(got[2])) > channelTolerance) {
                ++result.mismatches;
            }
        }
    }
    return result;
}

QByteArray shiftPixelsHorizontally(const QByteArray &pixels, int width, int height, int shift)
{
    QByteArray shifted(pixels.size(), char(0));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int sourceX = (x + shift) % width;
            const int destination = (y * width + x) * 4;
            const int source = (y * width + sourceX) * 4;
            for (int channel = 0; channel < 4; ++channel)
                shifted[destination + channel] = pixels[source + channel];
        }
    }
    return shifted;
}

struct Files {
    QTemporaryDir directory;
    RecordingFileReservation reservation;
    bool committed = false;

    Files()
    {
        const auto result = RecordingFileTransaction::reserve(
            directory.path(), QDateTime(QDate(2026, 7, 24), QTime(12, 0)),
            QUuid(QStringLiteral("{019f5a82-6320-73e3-90b7-7f2f0f6945a4}")));
        if (result.reservation) reservation = *result.reservation;
    }
    ~Files() { if (!committed) RecordingFileTransaction::discard(reservation); }
    QString commit()
    {
        const QString error = RecordingFileTransaction::commit(reservation);
        committed = error.isEmpty();
        return error;
    }
};

GstRecordingPipelineConfig config(const RecordingFileReservation &reservation,
                                  int width, int height, int fps)
{
    GstRecordingPipelineConfig result;
    result.videoSpoolPath = reservation.videoSpoolPath;
    result.audioSpoolPath = reservation.audioSpoolPath;
    result.temporaryMp4Path = reservation.temporaryMp4Path;
    result.video = {width, height, fps, 1, 1, 1};
    result.videoBitrateBitsPerSecond = recordingVideoBitrateBitsPerSecond(width, height, fps);
    result.preferredEncoder = QStringLiteral("openh264enc");
    return result;
}

GstRecordingPipeline realOpenH264Pipeline()
{
    GstRecordingPipelineHooks hooks;
    hooks.encoderBuildAllowed = [](const QString &name, QString *) {
        return name == QStringLiteral("openh264enc");
    };
    return GstRecordingPipeline(hooks);
}

struct DecodeTargets { GstElement *video = nullptr; GstElement *audio = nullptr; };

void linkDecodedPad(GstElement *, GstPad *pad, gpointer data)
{
    auto *targets = static_cast<DecodeTargets *>(data);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const QString name = capsName(caps);
    GstElement *target = name == QStringLiteral("video/x-raw") ? targets->video
        : (name == QStringLiteral("audio/x-raw") ? targets->audio : nullptr);
    if (target) {
        GstPad *sinkPad = gst_element_get_static_pad(target, "sink");
        if (!gst_pad_is_linked(sinkPad)) (void)gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
    }
    if (caps) gst_caps_unref(caps);
}

QString decodeDiagnostic(const QString &uri, const QString &track, GstSample *sample,
                         const QString &gerror = {})
{
    GstBuffer *buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    const GstSegment *segment = sample ? gst_sample_get_segment(sample) : nullptr;
    const GstClockTime rawPts = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
    const GstClockTime running = segment && GST_CLOCK_TIME_IS_VALID(rawPts)
        ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, rawPts) : GST_CLOCK_TIME_NONE;
    GstCaps *caps = sample ? gst_sample_get_caps(sample) : nullptr;
    gchar *capsText = caps ? gst_caps_to_string(caps) : nullptr;
    const QString result = QStringLiteral(
        "URI=%1; track=%2; GError=%3; raw PTS=%4; segment=%5; running-time=%6; caps=%7")
        .arg(uri, track, gerror.isEmpty() ? QStringLiteral("none") : gerror)
        .arg(rawPts == GST_CLOCK_TIME_NONE ? -1 : qint64(rawPts))
        .arg(segment ? QStringLiteral("format=%1,start=%2,time=%3,base=%4")
             .arg(int(segment->format)).arg(qint64(segment->start)).arg(qint64(segment->time))
             .arg(qint64(segment->base)) : QStringLiteral("none"))
        .arg(running == GST_CLOCK_TIME_NONE ? -1 : qint64(running))
        .arg(QString::fromUtf8(capsText ? capsText : "<none>"));
    g_free(capsText);
    return result;
}

QString takePipelineBusErrors(GstElement *pipeline)
{
    GstBus *bus = pipeline ? gst_element_get_bus(pipeline) : nullptr;
    if (!bus) return QStringLiteral("could not get pipeline bus");
    QStringList errors;
    while (GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
        GError *gerror = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &gerror, &debug);
        errors.append(QStringLiteral("GST_MESSAGE_ERROR=%1; debug=%2")
            .arg(QString::fromUtf8(gerror ? gerror->message : "unknown"),
                 QString::fromUtf8(debug ? debug : "<none>")));
        g_clear_error(&gerror);
        g_free(debug);
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    return errors.join(QStringLiteral(" | "));
}

GstClockTime pullRunningTime(GstElement *sink, const QString &uri, const QString &track,
                             QString *diagnostic)
{
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 15 * GST_SECOND);
    *diagnostic = decodeDiagnostic(uri, track, sample,
                                   sample ? QString() : QStringLiteral("no sample received"));
    if (!sample) return GST_CLOCK_TIME_NONE;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    const GstSegment *segment = gst_sample_get_segment(sample);
    const GstClockTime pts = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
    const GstClockTime result = segment && GST_CLOCK_TIME_IS_VALID(pts)
        ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, pts) : GST_CLOCK_TIME_NONE;
    gst_sample_unref(sample);
    return result;
}

bool firstDecodedRunningTimes(const QString &path, GstClockTime *video, GstClockTime *audio,
                              QString *error)
{
    const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    GstElement *pipeline = gst_pipeline_new("synthetic-running-time-decode");
    GstElement *decode = gst_element_factory_make("uridecodebin", nullptr);
    GstElement *videoQueue = gst_element_factory_make("queue", nullptr);
    GstElement *videoConvert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *videoSink = gst_element_factory_make("appsink", nullptr);
    GstElement *audioQueue = gst_element_factory_make("queue", nullptr);
    GstElement *audioConvert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *audioSink = gst_element_factory_make("appsink", nullptr);
    if (!pipeline || !decode || !videoQueue || !videoConvert || !videoSink || !audioQueue ||
        !audioConvert || !audioSink) {
        *error = QStringLiteral("URI=%1; GError=required decoder factory missing; raw PTS=-1; "
                                "segment=none; running-time=-1; caps=<none>").arg(uri);
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    g_object_set(decode, "uri", uri.toUtf8().constData(), nullptr);
    for (GstElement *sink : {videoSink, audioSink}) {
        g_object_set(sink, "drop", FALSE, "max-buffers", 1u, "sync", FALSE, nullptr);
    }
    gst_bin_add_many(GST_BIN(pipeline), decode, videoQueue, videoConvert, videoSink,
                     audioQueue, audioConvert, audioSink, nullptr);
    if (!gst_element_link_many(videoQueue, videoConvert, videoSink, nullptr) ||
        !gst_element_link_many(audioQueue, audioConvert, audioSink, nullptr)) {
        *error = QStringLiteral("URI=%1; GError=decoder branch link failed; raw PTS=-1; "
                                "segment=none; running-time=-1; caps=<none>").arg(uri);
        gst_object_unref(pipeline);
        return false;
    }
    DecodeTargets targets{videoQueue, audioQueue};
    g_signal_connect(decode, "pad-added", G_CALLBACK(linkDecodedPad), &targets);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *error = QStringLiteral("URI=%1; GError=PLAYING failed; raw PTS=-1; segment=none; "
                                "running-time=-1; caps=<none>").arg(uri);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    QString videoDiagnostic, audioDiagnostic;
    auto videoFuture = std::async(std::launch::async, [&] {
        return pullRunningTime(videoSink, uri, QStringLiteral("video"), &videoDiagnostic);
    });
    auto audioFuture = std::async(std::launch::async, [&] {
        return pullRunningTime(audioSink, uri, QStringLiteral("audio"), &audioDiagnostic);
    });
    *video = videoFuture.get();
    *audio = audioFuture.get();
    const QString busErrors = takePipelineBusErrors(pipeline);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    if (!GST_CLOCK_TIME_IS_VALID(*video) || !GST_CLOCK_TIME_IS_VALID(*audio)) {
        *error = videoDiagnostic + QStringLiteral(" | ") + audioDiagnostic +
                 QStringLiteral("; pipeline-bus=%1").arg(
                     busErrors.isEmpty() ? QStringLiteral("no GST_MESSAGE_ERROR") : busErrors);
        return false;
    }
    return true;
}

bool sampleNear(GstSample *sample, GstClockTime target, GstClockTime tolerance)
{
    GstBuffer *buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    const GstSegment *segment = sample ? gst_sample_get_segment(sample) : nullptr;
    if (!buffer || !segment || !GST_BUFFER_PTS_IS_VALID(buffer)) return false;
    const GstClockTime running = gst_segment_to_running_time(
        segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    return GST_CLOCK_TIME_IS_VALID(running) &&
           std::llabs(qint64(running) - qint64(target)) <= qint64(tolerance);
}

GstSample *decodeVideoNear(const QString &path, GstClockTime target, GstClockTime tolerance,
                           QString *error)
{
    const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    GstElement *pipeline = gst_pipeline_new("synthetic-video-decode");
    GstElement *decode = gst_element_factory_make("uridecodebin", nullptr);
    GstElement *queue = gst_element_factory_make("queue", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *sink = gst_element_factory_make("appsink", nullptr);
    if (!pipeline || !decode || !queue || !convert || !sink) {
        *error = QStringLiteral("URI=%1; GError=video decode factory missing; raw PTS=-1; "
                                "segment=none; running-time=-1; caps=<none>").arg(uri);
        if (pipeline) gst_object_unref(pipeline);
        return nullptr;
    }
    GstCaps *rgba = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", nullptr);
    gst_app_sink_set_caps(GST_APP_SINK(sink), rgba);
    gst_caps_unref(rgba);
    g_object_set(decode, "uri", uri.toUtf8().constData(), nullptr);
    g_object_set(sink, "drop", FALSE, "max-buffers", 4u, "sync", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), decode, queue, convert, sink, nullptr);
    if (!gst_element_link_many(queue, convert, sink, nullptr)) {
        *error = QStringLiteral("URI=%1; GError=video decode link failed; raw PTS=-1; "
                                "segment=none; running-time=-1; caps=<none>").arg(uri);
        gst_object_unref(pipeline);
        return nullptr;
    }
    DecodeTargets targets{queue, nullptr};
    g_signal_connect(decode, "pad-added", G_CALLBACK(linkDecodedPad), &targets);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *error = QStringLiteral("URI=%1; GError=video decode PLAYING failed; raw PTS=-1; "
                                "segment=none; running-time=-1; caps=<none>").arg(uri);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return nullptr;
    }
    GstSample *result = nullptr;
    for (int index = 0; index < 180; ++index) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND);
        if (!sample) break;
        if (sampleNear(sample, target, tolerance)) { result = sample; break; }
        gst_sample_unref(sample);
    }
    if (!result) {
        const QString busErrors = takePipelineBusErrors(pipeline);
        *error = decodeDiagnostic(uri, QStringLiteral("video"), result,
                                  busErrors.isEmpty()
                                      ? QStringLiteral("no sample received; no GST_MESSAGE_ERROR")
                                      : busErrors);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return result;
}

bool blackFrame(GstSample *sample)
{
    GstBuffer *buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    if (!buffer) return false;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return false;
    quint64 total = 0;
    for (gsize index = 0; index + 3 < map.size; index += 4)
        total += map.data[index] + map.data[index + 1] + map.data[index + 2];
    const gsize size = map.size;
    gst_buffer_unmap(buffer, &map);
    return total < size * 10u;
}

void enqueueVideo(RecordingController &controller, GstSample *sample)
{
    QElapsedTimer wait;
    wait.start();
    while (!controller.tryEnqueueVideoSample(sample) && wait.elapsed() < 10'000) QTest::qWait(1);
    QVERIFY2(wait.elapsed() < 10'000, "video controller queue did not accept sample");
    gst_sample_unref(sample);
}

RecordingResult finish(RecordingController &controller, QSignalSpy *finished, QSignalSpy *failed)
{
    controller.stop();
    QElapsedTimer timeout;
    timeout.start();
    while (finished->count() == 0 && failed->count() == 0 && timeout.elapsed() < 120'000)
        QTest::qWait(10);
    if (finished->count() != 1 || !failed->isEmpty()) {
        const QString message = failed->isEmpty()
            ? QStringLiteral("recording did not finish before timeout")
            : failed->at(0).at(0).toString();
        QTest::qFail(qPrintable(message), __FILE__, __LINE__);
        return {};
    }
    return qvariant_cast<RecordingResult>(finished->at(0).at(0));
}

RecordingControllerHooks openH264Hooks()
{
    RecordingControllerHooks hooks;
    hooks.probeCapabilities = [] { return GstRecordingCapabilityResult{
        true, QStringLiteral("openh264enc"), {}}; };
    return hooks;
}

} // namespace

class RecordingSyntheticMp4Test : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }

    void bitrateMetadataUsesBitsPerSecond()
    {
        QVERIFY(reportedBitrateInTier(4'000'000, 4'000'000));
        QVERIFY(!reportedBitrateInTier(500'000, 4'000'000));
    }

    void videoOnly540p30ThroughControllerIsDiscoverable()
    {
        QTemporaryDir output;
        RecordingController controller(30, openH264Hooks());
        GstSample *probe = videoSample(960, 540, 30, 0);
        QVERIFY(!controller.tryEnqueueVideoSample(probe));
        gst_sample_unref(probe);
        QTRY_VERIFY(controller.available());
        const RecordingStartResult start = controller.start({output.path(), RecordingFormat::Mp4});
        QVERIFY2(start.accepted, qPrintable(start.error));
        QSignalSpy finished(&controller, &RecordingController::finished);
        QSignalSpy failed(&controller, &RecordingController::failed);
        QVector<GstSample *> frames;
        frames.reserve(90);
        for (int frame = 0; frame < 90; ++frame) {
            frames.append(busyVideoSample(
                960, 540, 30, frame * GST_SECOND / 30, 0x540000u + quint32(frame), 24));
        }
        for (GstSample *frame : frames) enqueueVideo(controller, frame);
        const RecordingResult result = finish(controller, &finished, &failed);
        QVERIFY(QFileInfo::exists(result.finalPath));
        MediaSummary media;
        QString error;
        QVERIFY2(discover(result.finalPath, &media, &error), qPrintable(error));
        QCOMPARE(media.video.size(), 1);
        QCOMPARE(media.audio.size(), 0);
        QVERIFY(media.container.contains(QStringLiteral("quicktime"), Qt::CaseInsensitive) ||
                media.container.contains(QStringLiteral("iso"), Qt::CaseInsensitive));
        QVERIFY(media.video.first().codec.contains(QStringLiteral("h264"), Qt::CaseInsensitive));
        QCOMPARE(media.video.first().width, 960);
        QCOMPARE(media.video.first().height, 540);
        QCOMPARE(media.video.first().fpsNumerator, 30);
        QCOMPARE(media.video.first().fpsDenominator, 1);
        QVERIFY(std::llabs(qint64(media.duration) - 3 * qint64(GST_SECOND)) <= 100 * qint64(GST_MSECOND));
        QCOMPARE(recordingVideoBitrateBitsPerSecond(960, 540, 30), 4'000'000);
        const int observed = reportedVideoBitrate(media);
        QVERIFY2(reportedBitrateInTier(observed, 4'000'000),
                 qPrintable(QStringLiteral("unexpected 540p reported bitrate: %1")
                            .arg(bitrateDiagnostic(media))));
        controller.acknowledgeResult();
    }

    void lateAudio720p60PreservesDecodedStartOffset()
    {
        Files files;
        QVERIFY(!files.reservation.finalPath.isEmpty());
        GstRecordingPipeline pipeline = realOpenH264Pipeline();
        QString error;
        GstSample *first = busyVideoSample(1280, 720, 60, 10 * GST_SECOND, 0x720000u, 48);
        QVERIFY2(pipeline.start(config(files.reservation, 1280, 720, 60), first, &error), qPrintable(error));
        gst_sample_unref(first);
        for (int frame = 1; frame < 180; ++frame) {
            GstSample *sample = busyVideoSample(
                1280, 720, 60, 10 * GST_SECOND + frame * GST_SECOND / 60,
                0x720000u + quint32(frame), 48);
            QVERIFY2(pipeline.pushVideo(sample, frame * GST_SECOND / 60, &error), qPrintable(error));
            gst_sample_unref(sample);
        }
        for (int frame = 0; frame < 225; ++frame) {
            GstSample *sample = audioSample(10 * GST_SECOND + 750 * GST_MSECOND + frame * 10 * GST_MSECOND);
            QVERIFY2(pipeline.pushAudio(sample, 750 * GST_MSECOND + frame * 10 * GST_MSECOND, &error), qPrintable(error));
            gst_sample_unref(sample);
        }
        std::atomic_bool cancelled{false};
        const auto finalized = pipeline.finalize(60'000, cancelled);
        QVERIFY2(finalized.success, qPrintable(finalized.error));
        QVERIFY2(files.commit().isEmpty(), "could not commit synthetic MP4");
        MediaSummary media;
        QVERIFY2(discover(files.reservation.finalPath, &media, &error), qPrintable(error));
        QCOMPARE(media.video.size(), 1);
        QCOMPARE(media.audio.size(), 1);
        QVERIFY(media.video.first().codec.contains(QStringLiteral("h264"), Qt::CaseInsensitive));
        QVERIFY(media.audio.first().codec.contains(QStringLiteral("aac"), Qt::CaseInsensitive) ||
                media.audio.first().codec == QStringLiteral("audio/mpeg"));
        QCOMPARE(media.video.first().width, 1280);
        QCOMPARE(media.video.first().height, 720);
        QCOMPARE(media.video.first().fpsNumerator, 60);
        QCOMPARE(media.video.first().fpsDenominator, 1);
        QCOMPARE(recordingVideoBitrateBitsPerSecond(1280, 720, 60), 9'000'000);
        const int observed = reportedVideoBitrate(media);
        QVERIFY2(reportedBitrateInTier(observed, 9'000'000),
                 qPrintable(QStringLiteral("unexpected 720p reported bitrate: %1")
                            .arg(bitrateDiagnostic(media))));
        const int audioBitrate = media.audio.first().bitrate > 0
            ? media.audio.first().bitrate : media.audio.first().taggedBitrate;
        QVERIFY2(reportedBitrateInTier(audioBitrate, 192'000, 50, 150),
                 qPrintable(QStringLiteral("unexpected AAC reported bitrate %1 (stream=%2; tag=%3)")
                            .arg(audioBitrate).arg(media.audio.first().bitrate)
                            .arg(media.audio.first().taggedBitrate)));
        GstClockTime videoStart = GST_CLOCK_TIME_NONE, audioStart = GST_CLOCK_TIME_NONE;
        QVERIFY2(firstDecodedRunningTimes(files.reservation.finalPath, &videoStart, &audioStart, &error),
                 qPrintable(error));
        QVERIFY(std::llabs(qint64(videoStart)) <= 100 * qint64(GST_MSECOND));
        QVERIFY(std::llabs(qint64(audioStart) - 750 * qint64(GST_MSECOND)) <= 100 * qint64(GST_MSECOND));
    }

    void blackPauseRecoveryRetainsSourceGap()
    {
        Files files;
        GstRecordingPipeline pipeline = realOpenH264Pipeline();
        QString error;
        GstSample *first = videoSample(320, 180, 30, 0);
        QVERIFY2(pipeline.start(config(files.reservation, 320, 180, 30), first, &error), qPrintable(error));
        gst_sample_unref(first);
        for (int frame = 1; frame <= 15; ++frame) {
            GstSample *sample = videoSample(320, 180, 30, frame * GST_SECOND / 30);
            QVERIFY(pipeline.pushVideo(sample, frame * GST_SECOND / 30, &error));
            gst_sample_unref(sample);
        }
        for (int frame = 16; frame < 54; ++frame)
            QVERIFY(pipeline.pushBlackFrame(frame * GST_SECOND / 30, &error));
        for (int frame = 54; frame < 90; ++frame) {
            QByteArray bright(320 * 180 * 4, char(0));
            for (int index = 0; index < bright.size(); index += 4) {
                bright[index] = char(220); bright[index + 1] = char(180); bright[index + 2] = char(40);
                bright[index + 3] = char(255);
            }
            GstSample *sample = videoSample(320, 180, 30, frame * GST_SECOND / 30, bright);
            QVERIFY(pipeline.pushVideo(sample, frame * GST_SECOND / 30, &error));
            gst_sample_unref(sample);
        }
        std::atomic_bool cancelled{false};
        QVERIFY2(pipeline.finalize(60'000, cancelled).success, qPrintable(error));
        QVERIFY2(files.commit().isEmpty(), "could not commit black-frame MP4");
        GstSample *gap = decodeVideoNear(files.reservation.finalPath, 1550 * GST_MSECOND, 80 * GST_MSECOND, &error);
        QVERIFY2(gap, qPrintable(error));
        QVERIFY(blackFrame(gap));
        gst_sample_unref(gap);
        GstSample *recovered = decodeVideoNear(files.reservation.finalPath, 2200 * GST_MSECOND, 80 * GST_MSECOND, &error);
        QVERIFY2(recovered, qPrintable(error));
        QVERIFY(!blackFrame(recovered));
        gst_sample_unref(recovered);
        MediaSummary media;
        QVERIFY2(discover(files.reservation.finalPath, &media, &error), qPrintable(error));
        QVERIFY(std::llabs(qint64(media.duration) - 3 * qint64(GST_SECOND)) <= 100 * qint64(GST_MSECOND));
    }

    void checkerboardIsOneToOneAndDimensionChangeSavesWarning()
    {
        Files files;
        constexpr int checkerWidth = 160;
        constexpr int checkerHeight = 120;
        constexpr int checkerCellSize = 4;
        constexpr int rgbTolerance = 84;
        constexpr int maxPixelMismatches = 3'000;
        constexpr quint8 palette[8][3] = {
            {245, 20, 20}, {105, 70, 20},
            {20, 245, 20}, {70, 105, 20},
            {20, 20, 245}, {20, 70, 105},
            {245, 245, 20}, {105, 105, 20},
        };
        const auto paletteIndex = [](int x, int y) {
            const quint32 cellX = static_cast<quint32>(x / checkerCellSize);
            const quint32 cellY = static_cast<quint32>(y / checkerCellSize);
            quint32 coordinateHash = cellX * 0x9e3779b9u ^ cellY * 0x85ebca6bu;
            coordinateHash ^= coordinateHash >> 16;
            coordinateHash *= 0x7feb352du;
            coordinateHash ^= coordinateHash >> 15;
            const quint32 baseColor = (cellX & 1u) | ((cellY & 1u) << 1u);
            return int(baseColor * 2u + (coordinateHash & 1u));
        };
        QByteArray checker(checkerWidth * checkerHeight * 4, char(0));
        for (int y = 0; y < checkerHeight; ++y) for (int x = 0; x < checkerWidth; ++x) {
            const int offset = (y * checkerWidth + x) * 4;
            const quint8 *color = palette[paletteIndex(x, y)];
            checker[offset] = char(color[0]);
            checker[offset + 1] = char(color[1]);
            checker[offset + 2] = char(color[2]);
            checker[offset + 3] = char(255);
        }
        const auto onePixelShift = shiftPixelsHorizontally(
            checker, checkerWidth, checkerHeight, 1);
        const auto oldCheckerPeriodShift = shiftPixelsHorizontally(
            checker, checkerWidth, checkerHeight, 16);
        const RgbComparison onePixelNegative = compareRgbPixels(
            checker, reinterpret_cast<const quint8 *>(onePixelShift.constData()),
            checkerWidth, checkerHeight, checkerWidth * 4, 0);
        const RgbComparison oldCheckerPeriodNegative = compareRgbPixels(
            checker, reinterpret_cast<const quint8 *>(oldCheckerPeriodShift.constData()),
            checkerWidth, checkerHeight, checkerWidth * 4, 0);
        QVERIFY2(onePixelNegative.mismatches > maxPixelMismatches,
                 "pixel-fidelity threshold must reject a one-pixel translation");
        QVERIFY2(oldCheckerPeriodNegative.mismatches > maxPixelMismatches,
                 "pixel-fidelity threshold must reject the old checker period translation");
        GstRecordingPipeline pipeline = realOpenH264Pipeline();
        QString error;
        GstSample *first = videoSample(checkerWidth, checkerHeight, 30, 0, checker);
        QVERIFY2(pipeline.start(config(files.reservation, checkerWidth, checkerHeight, 30), first, &error), qPrintable(error));
        gst_sample_unref(first);
        for (int frame = 1; frame < 30; ++frame) {
            GstSample *sample = videoSample(checkerWidth, checkerHeight, 30, frame * GST_SECOND / 30, checker);
            QVERIFY(pipeline.pushVideo(sample, frame * GST_SECOND / 30, &error));
            gst_sample_unref(sample);
        }
        std::atomic_bool cancelled{false};
        QVERIFY2(pipeline.finalize(30'000, cancelled).success, qPrintable(error));
        QVERIFY2(files.commit().isEmpty(), "could not commit checkerboard MP4");
        MediaSummary media;
        QVERIFY2(discover(files.reservation.finalPath, &media, &error), qPrintable(error));
        QCOMPARE(media.video.first().width, checkerWidth);
        QCOMPARE(media.video.first().height, checkerHeight);
        QCOMPARE(media.video.first().parNumerator, 1);
        QCOMPARE(media.video.first().parDenominator, 1);
        GstSample *decoded = decodeVideoNear(files.reservation.finalPath, 0, 80 * GST_MSECOND, &error);
        QVERIFY2(decoded, qPrintable(error));
        GstMapInfo map;
        QVERIFY(gst_buffer_map(gst_sample_get_buffer(decoded), &map, GST_MAP_READ));
        QVERIFY(map.size >= static_cast<gsize>(checkerWidth * checkerHeight * 4));
        const gsize rowStride = map.size / checkerHeight;
        QVERIFY(rowStride >= static_cast<gsize>(checkerWidth * 4));
        const RgbComparison decodedComparison = compareRgbPixels(
            checker, map.data, checkerWidth, checkerHeight, rowStride, rgbTolerance);
        QVERIFY2(decodedComparison.mismatches <= maxPixelMismatches,
                 qPrintable(QStringLiteral("RGB mismatch count %1/%2 (tolerance=%3; 1px=%4; "
                                          "old-period=%5): crop, scale, or alignment changed")
                            .arg(decodedComparison.mismatches).arg(decodedComparison.total).arg(rgbTolerance)
                            .arg(onePixelNegative.mismatches)
                            .arg(oldCheckerPeriodNegative.mismatches)));
        gst_buffer_unmap(gst_sample_get_buffer(decoded), &map);
        gst_sample_unref(decoded);

        QTemporaryDir output;
        RecordingController controller(30, openH264Hooks());
        GstSample *probe = videoSample(checkerWidth, checkerHeight, 30, 0, checker);
        QVERIFY(!controller.tryEnqueueVideoSample(probe));
        gst_sample_unref(probe);
        QTRY_VERIFY(controller.available());
        QVERIFY(controller.start({output.path(), RecordingFormat::Mp4}).accepted);
        QSignalSpy finished(&controller, &RecordingController::finished);
        QSignalSpy failed(&controller, &RecordingController::failed);
        for (int frame = 0; frame < 30; ++frame) {
            enqueueVideo(controller, videoSample(
                checkerWidth, checkerHeight, 30, frame * GST_SECOND / 30, checker));
        }
        enqueueVideo(controller, videoSample(200, checkerHeight, 30, GST_SECOND));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1 || failed.count() == 1, 60'000);
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.at(0).at(0).toString()));
        const RecordingResult changed = qvariant_cast<RecordingResult>(finished.at(0).at(0));
        QVERIFY(QFileInfo::exists(changed.finalPath));
        QVERIFY(changed.warning.contains(QStringLiteral("dimensions changed"), Qt::CaseInsensitive));
        MediaSummary changedMedia;
        QVERIFY2(discover(changed.finalPath, &changedMedia, &error), qPrintable(error));
        QCOMPARE(changedMedia.video.size(), 1);
        QCOMPARE(changedMedia.audio.size(), 0);
        QCOMPARE(changedMedia.video.first().width, checkerWidth);
        QCOMPARE(changedMedia.video.first().height, checkerHeight);
        QCOMPARE(changedMedia.video.first().parNumerator, 1);
        QCOMPARE(changedMedia.video.first().parDenominator, 1);
        GstSample *prefix = decodeVideoNear(changed.finalPath, 0, 80 * GST_MSECOND, &error);
        QVERIFY2(prefix, qPrintable(error));
        gst_sample_unref(prefix);
        controller.acknowledgeResult();
    }

    void mfhEncoderProducesDiscoverableMp4OrSkipsWithoutGpu()
    {
        if (!qEnvironmentVariableIsSet("AIRPLAY_RUN_MFH_RECORDING_TEST")) {
            QSKIP("mfh264enc recording requires an explicitly enabled usable GPU; "
                  "set AIRPLAY_RUN_MFH_RECORDING_TEST=1 to run it");
        }
        GstElementFactory *mfh = gst_element_factory_find("mfh264enc");
        if (!mfh) QSKIP("mfh264enc is not installed on this host");
        gst_object_unref(mfh);
        Files files;
        GstRecordingPipeline pipeline;
        QString error;
        GstSample *first = videoSample(160, 120, 30, 0);
        auto mfhConfig = config(files.reservation, 160, 120, 30);
        mfhConfig.preferredEncoder = QStringLiteral("mfh264enc");
        if (!pipeline.start(mfhConfig, first, &error)) {
            gst_sample_unref(first);
            QSKIP(qPrintable(QStringLiteral("mfh264enc cannot run without a usable GPU: %1").arg(error)));
        }
        gst_sample_unref(first);
        for (int frame = 1; frame < 30; ++frame) {
            GstSample *sample = videoSample(160, 120, 30, frame * GST_SECOND / 30);
            QVERIFY2(pipeline.pushVideo(sample, frame * GST_SECOND / 30, &error), qPrintable(error));
            gst_sample_unref(sample);
        }
        std::atomic_bool cancelled{false};
        const auto result = pipeline.finalize(30'000, cancelled);
        if (!result.success) {
            QSKIP(qPrintable(QStringLiteral("mfh264enc finalization unavailable without GPU: %1")
                             .arg(result.error)));
        }
        QVERIFY2(files.commit().isEmpty(), "mfh MP4 could not be committed");
        MediaSummary media;
        QVERIFY2(discover(files.reservation.finalPath, &media, &error), qPrintable(error));
        QCOMPARE(media.video.size(), 1);
        QVERIFY(media.video.first().codec.contains(QStringLiteral("h264"), Qt::CaseInsensitive));
    }

    void encoderAndFailureMatrixHasOpenH264Coverage()
    {
        const auto factoryHooks = [](const QSet<QString> &present) {
            GstRecordingPipelineHooks hooks;
            hooks.factoryExists = [present](const QString &name) { return present.contains(name); };
            hooks.factoryReady = [](const QString &, QString *) { return true; };
            return hooks;
        };
        const QSet<QString> all{QStringLiteral("matroskamux"), QStringLiteral("matroskademux"),
            QStringLiteral("mp4mux"), QStringLiteral("h264parse"), QStringLiteral("avenc_aac"),
            QStringLiteral("aacparse"), QStringLiteral("appsrc"), QStringLiteral("appsink"),
            QStringLiteral("videoconvert"), QStringLiteral("audioconvert"),
            QStringLiteral("audioresample"), QStringLiteral("capsfilter"), QStringLiteral("filesrc"),
            QStringLiteral("filesink"), QStringLiteral("identity"), QStringLiteral("mfh264enc"),
            QStringLiteral("openh264enc")};
        const auto mfh = GstRecordingPipeline::probeCapabilities(factoryHooks(all));
        QVERIFY(mfh.available);
        QCOMPARE(mfh.preferredEncoder, QStringLiteral("mfh264enc"));
        auto mfhFail = factoryHooks(all);
        mfhFail.factoryReady = [](const QString &name, QString *error) {
            if (name == QStringLiteral("mfh264enc")) { *error = QStringLiteral("forced mfh failure"); return false; }
            return true;
        };
        const auto fallback = GstRecordingPipeline::probeCapabilities(mfhFail);
        QVERIFY(fallback.available);
        QCOMPARE(fallback.preferredEncoder, QStringLiteral("openh264enc"));
        QSet<QString> noEncoders = all;
        noEncoders.remove(QStringLiteral("mfh264enc")); noEncoders.remove(QStringLiteral("openh264enc"));
        QVERIFY(!GstRecordingPipeline::probeCapabilities(factoryHooks(noEncoders)).available);
        QSet<QString> noAac = all; noAac.remove(QStringLiteral("avenc_aac"));
        const auto aacMissing = GstRecordingPipeline::probeCapabilities(factoryHooks(noAac));
        QVERIFY(!aacMissing.available);
        QVERIFY(aacMissing.error.contains(QStringLiteral("AAC")));
        QSet<QString> noMux = all; noMux.remove(QStringLiteral("mp4mux"));
        const auto muxMissing = GstRecordingPipeline::probeCapabilities(factoryHooks(noMux));
        QVERIFY(!muxMissing.available);
        QVERIFY(muxMissing.error.contains(QStringLiteral("MP4")));

        Files diskFailure;
        GstRecordingPipeline pipeline = realOpenH264Pipeline();
        QString error;
        auto invalid = config(diskFailure.reservation, 64, 48, 30);
        invalid.videoSpoolPath = QDir::rootPath() + QStringLiteral("airplay-recording-unwritable/video.mkv");
        GstSample *sample = videoSample(64, 48, 30, 0);
        QVERIFY(!pipeline.start(invalid, sample, &error));
        gst_sample_unref(sample);
        QVERIFY2(!error.isEmpty(), "disk-write failure must report a pipeline error");
        GstElementFactory *openh264 = gst_element_factory_find("openh264enc");
        QVERIFY2(openh264, "OpenH264 must not be skipped in the synthetic MP4 suite");
        gst_object_unref(openh264);
    }
};

QTEST_GUILESS_MAIN(RecordingSyntheticMp4Test)
#include "RecordingSyntheticMp4Test.moc"

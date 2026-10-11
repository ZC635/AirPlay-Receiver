#include <QtTest/QtTest>

#if AIRPLAY_WITH_UXPLAY
#include <algorithm>
#include <memory>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "lib/logger.h"
#include "renderers/audio_renderer.h"
#include "renderers/video_renderer.h"
#endif

class VideoRendererLifecycleTest : public QObject {
    Q_OBJECT

private slots:
    void reconnectDeliversFreshDecodedFrames_data() {
        QTest::addColumn<bool>("recordingTap");
        QTest::newRow("display-only") << false;
        QTest::newRow("display-and-recording") << true;
    }

    void reconnectDeliversFreshDecodedFrames() {
#if AIRPLAY_WITH_UXPLAY
        QFETCH(bool, recordingTap);
        QVERIFY(gstreamer_init());
        for (const char *name : {"videotestsrc", "openh264enc", "h264parse",
                                 "decodebin", "videoconvert", "appsink"}) {
            GstElementFactory *factory = gst_element_factory_find(name);
            QVERIFY2(factory, qPrintable(QStringLiteral("Required test plugin: %1")
                                            .arg(QString::fromLatin1(name))));
            gst_object_unref(factory);
        }
        // Independent all-intra input gives each connection its own SPS/PPS and pixels.
        const auto encode = [](const char *pattern) {
            QList<QByteArray> units;
            const QByteArray description = QByteArray("videotestsrc num-buffers=3 pattern=") +
                pattern + " ! video/x-raw,format=I420,width=64,height=64,framerate=30/1 ! "
                "openh264enc gop-size=1 ! h264parse config-interval=-1 ! "
                "video/x-h264,stream-format=byte-stream,alignment=au ! "
                "appsink name=encoded_sink sync=false";
            GError *error = nullptr;
            GstElement *pipeline = gst_parse_launch(description.constData(), &error);
            if (error || !pipeline) {
                if (error) g_error_free(error);
                if (pipeline) gst_object_unref(pipeline);
                return units;
            }
            GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "encoded_sink");
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            for (int i = 0; i < 3; ++i) {
                GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                                5 * GST_SECOND);
                if (!sample) break;
                GstMapInfo map;
                GstBuffer *buffer = gst_sample_get_buffer(sample);
                if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    units.append(QByteArray(reinterpret_cast<const char *>(map.data),
                                            static_cast<int>(map.size)));
                    gst_buffer_unmap(buffer, &map);
                }
                gst_sample_unref(sample);
            }
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(sink);
            gst_object_unref(pipeline);
            return units;
        };
        const QList<QByteArray> red = encode("red");
        const QList<QByteArray> blue = encode("blue");
        QCOMPARE(red.size(), 3);
        QCOMPARE(blue.size(), 3);
        auto logger = std::unique_ptr<logger_t, decltype(&logger_destroy)>(
            logger_init(), logger_destroy);
        QVERIFY(logger != nullptr);
        logger_set_callback(logger.get(), [](void *, int, const char *) {}, nullptr);
        struct RendererCleanup {
            ~RendererCleanup() {
                video_renderer_set_sample_callback(nullptr, nullptr);
                video_renderer_destroy();
            }
        } cleanup;
        if (recordingTap)
            video_renderer_set_sample_callback([](GstSample *, void *) {}, nullptr);
        videoflip_t flip[2] = {NONE, NONE};
        QCOMPARE(video_renderer_init(logger.get(), "Reconnect Frame Test", flip,
                                      "h264parse", "", "decodebin", "videoconvert",
                                      "appsink", "", false, false, false, false,
                                      3, nullptr), 0);
        for (int cycle = 0; cycle < 3; ++cycle) {
            video_renderer_start();
            QCOMPARE(video_renderer_choose_codec(false, false), 0);
            GstElement *pipeline = static_cast<GstElement *>(video_renderer_get_pipeline());
            QVERIFY(pipeline != nullptr);
            auto sink = std::unique_ptr<GstElement, decltype(&gst_object_unref)>(
                gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h264"), gst_object_unref);
            QVERIFY(sink != nullptr);
            const auto &units = cycle % 2 == 0 ? red : blue;
            for (int i = 0; i < units.size(); ++i) {
                QByteArray bytes = units.at(i);
                int length = bytes.size();
                int nalCount = 1;
                uint64_t pts = 10 * GST_SECOND + gst_util_uint64_scale(i, GST_SECOND, 30);
                QCOMPARE(video_renderer_render_buffer(
                             reinterpret_cast<unsigned char *>(bytes.data()),
                             &length, &nalCount, &pts), uint64_t(0));
            }
            auto sample = std::unique_ptr<GstSample, decltype(&gst_sample_unref)>(
                gst_app_sink_try_pull_sample(GST_APP_SINK(sink.get()), 5 * GST_SECOND),
                gst_sample_unref);
            QVERIFY2(sample != nullptr, qPrintable(QStringLiteral(
                "Connection %1 must deliver decoded video after stop/start").arg(cycle + 1)));
            GstCaps *caps = gst_sample_get_caps(sample.get());
            QVERIFY(caps != nullptr);
            const GstStructure *shape = gst_caps_get_structure(caps, 0);
            QCOMPARE(QByteArray(gst_structure_get_string(shape, "format")), QByteArray("RGBA"));
            int width = 0, height = 0;
            QVERIFY(gst_structure_get_int(shape, "width", &width));
            QVERIFY(gst_structure_get_int(shape, "height", &height));
            QCOMPARE(width, 64);
            QCOMPARE(height, 64);
            GstBuffer *buffer = gst_sample_get_buffer(sample.get());
            GstMapInfo map;
            QVERIFY(gst_buffer_map(buffer, &map, GST_MAP_READ));
            const bool freshColor = map.size >= 4 &&
                (cycle % 2 == 0 ? map.data[0] > 200 && map.data[2] < 50
                                : map.data[2] > 200 && map.data[0] < 50);
            gst_buffer_unmap(buffer, &map);
            QVERIFY2(freshColor, "Decoded pixels must come from the current connection");
            video_renderer_stop();
        }
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void tapRegistrationCanBeReestablishedAcrossRendererRecreation() {
#if AIRPLAY_WITH_UXPLAY
        if (!gstreamer_init()) {
            QSKIP("GStreamer is not available in this environment");
        }
        auto logger = std::unique_ptr<logger_t, decltype(&logger_destroy)>(
            logger_init(), logger_destroy);
        QVERIFY(logger != nullptr);
        logger_set_callback(logger.get(), [](void *, int, const char *) {}, nullptr);
        struct RendererCleanup {
            ~RendererCleanup() {
                video_renderer_set_sample_callback(nullptr, nullptr);
                video_renderer_destroy();
            }
        } cleanup;
        const auto tap = [](GstSample *, void *) {};
        const auto verifySelectedPipeline = [](const char *displayName,
                                               const char *recordingName) {
            GstElement *pipeline = static_cast<GstElement *>(
                video_renderer_get_pipeline());
            QVERIFY(pipeline != nullptr);
            GstElement *display = gst_bin_get_by_name(GST_BIN(pipeline), displayName);
            GstElement *recording = gst_bin_get_by_name(GST_BIN(pipeline), recordingName);
            QVERIFY(display != nullptr);
            QVERIFY(recording != nullptr);
            gst_object_unref(display);
            gst_object_unref(recording);
        };
        videoflip_t videoFlip[2] = {NONE, NONE};

        video_renderer_set_sample_callback(tap, nullptr);
        QCOMPARE(video_renderer_init(logger.get(), "Tap Recreate H264", videoFlip,
                                      "h264parse", "", "decodebin", "videoconvert",
                                      "appsink", "", false, false, true, false,
                                      3, nullptr), 0);
        video_renderer_start();
        QCOMPARE(video_renderer_choose_codec(false, false), 0);
        verifySelectedPipeline("appsink_h264", "recording_video_sink_h264");
        video_renderer_stop();
        video_renderer_set_sample_callback(nullptr, nullptr);
        video_renderer_destroy();

        video_renderer_set_sample_callback(tap, nullptr);
        QCOMPARE(video_renderer_init(logger.get(), "Tap Recreate H265", videoFlip,
                                      "h264parse", "", "decodebin", "videoconvert",
                                      "appsink", "", false, false, true, false,
                                      3, nullptr), 0);
        video_renderer_start();
        QCOMPARE(video_renderer_choose_codec(false, true), 0);
        verifySelectedPipeline("appsink_h265", "recording_video_sink_h265");
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void initReturnsErrorOnInvalidPlaybinVersion() {
#if AIRPLAY_WITH_UXPLAY
        if (!gstreamer_init()) {
            QSKIP("GStreamer is not available in this environment");
        }

        QStringList messages;
        auto logger = std::unique_ptr<logger_t, decltype(&logger_destroy)>(logger_init(), logger_destroy);
        QVERIFY(logger != nullptr);
        logger_set_level(logger.get(), LOGGER_DEBUG);
        logger_set_callback(logger.get(), [](void *cls, int, const char *message) {
            auto *messages = static_cast<QStringList *>(cls);
            messages->append(QString::fromUtf8(message ? message : ""));
        }, &messages);

        struct RendererCleanup {
            ~RendererCleanup() { video_renderer_destroy(); }
        } cleanup;

        videoflip_t videoFlip[2] = {NONE, NONE};
        const int result = video_renderer_init(logger.get(), "Bad Playbin Test", videoFlip, "h264parse", "",
                                                "decodebin", "videoconvert", "fakesink", "", false, false, false, false,
                                                99, "http://fake-hls-stream.example/test.m3u8");
        QVERIFY2(result != 0, "video_renderer_init must return non-zero for invalid playbin version");
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void choosingSameCodecAfterStopRestartsVideoPipeline() {
#if AIRPLAY_WITH_UXPLAY
        if (!gstreamer_init()) {
            QSKIP("GStreamer is not available in this environment");
        }

        QStringList messages;
        auto logger = std::unique_ptr<logger_t, decltype(&logger_destroy)>(logger_init(), logger_destroy);
        QVERIFY(logger != nullptr);
        logger_set_level(logger.get(), LOGGER_DEBUG);
        logger_set_callback(logger.get(), [](void *cls, int, const char *message) {
            auto *messages = static_cast<QStringList *>(cls);
            messages->append(QString::fromUtf8(message ? message : ""));
        }, &messages);

        struct RendererCleanup {
            ~RendererCleanup() { video_renderer_destroy(); }
        } cleanup;

        videoflip_t videoFlip[2] = {NONE, NONE};
        QCOMPARE(video_renderer_init(logger.get(), "Video Renderer Lifecycle Test", videoFlip, "h264parse", "",
                                      "decodebin", "videoconvert", "fakesink", "", false, false, false, false,
                                      3, nullptr), 0);
        video_renderer_start();

        auto stateChangeLogCount = [&messages] {
            return std::count_if(messages.cbegin(), messages.cend(), [](const QString &message) {
                return message.contains("video_pipeline state change");
            });
        };

        QCOMPARE(video_renderer_choose_codec(false, false), 0);
        const auto firstStartLogCount = stateChangeLogCount();
        QVERIFY(firstStartLogCount > 0);

        video_renderer_stop();
        QVERIFY2(std::any_of(messages.cbegin(), messages.cend(), [](const QString &message) {
                     return message.contains("video_renderer_stop: state NULL");
                 }),
                 "Stopping must wait until the active video pipeline reaches GST_STATE_NULL");

        QCOMPARE(video_renderer_choose_codec(false, false), 0);
        QVERIFY2(stateChangeLogCount() > firstStartLogCount,
                 "Choosing the same codec after stop must restart the selected video pipeline");
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void startAfterCodecSelectionSkipsDestroyedRendererSlots() {
#if AIRPLAY_WITH_UXPLAY
        if (!gstreamer_init()) {
            QSKIP("GStreamer is not available in this environment");
        }

        auto logger = std::unique_ptr<logger_t, decltype(&logger_destroy)>(logger_init(), logger_destroy);
        QVERIFY(logger != nullptr);
        logger_set_level(logger.get(), LOGGER_DEBUG);
        logger_set_callback(logger.get(), [](void *, int, const char *) {}, nullptr);

        struct RendererCleanup {
            ~RendererCleanup() { video_renderer_destroy(); }
        } cleanup;

        videoflip_t videoFlip[2] = {NONE, NONE};
        QCOMPARE(video_renderer_init(logger.get(), "Video Renderer Reconnect Test", videoFlip, "h264parse", "",
                                      "decodebin", "videoconvert", "fakesink", "", false, false, false, true,
                                      3, nullptr), 0);
        video_renderer_start();
        QCOMPARE(video_renderer_choose_codec(false, false), 0);

        video_renderer_stop();
        video_renderer_start();
        QCOMPARE(video_renderer_choose_codec(false, false), 0);
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }
};

QTEST_MAIN(VideoRendererLifecycleTest)
#include "VideoRendererLifecycleTest.moc"

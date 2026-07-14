#include <QtTest/QtTest>

#if AIRPLAY_WITH_UXPLAY
#include <algorithm>
#include <memory>

#include <gst/gst.h>

#include "lib/logger.h"
#include "renderers/audio_renderer.h"
#include "renderers/video_renderer.h"
#endif

class VideoRendererLifecycleTest : public QObject {
    Q_OBJECT

private slots:
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

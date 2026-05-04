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
        video_renderer_init(logger.get(), "Video Renderer Lifecycle Test", videoFlip, "h264parse", "",
                            "decodebin", "videoconvert", "fakesink", "", false, false, false, false, 3, nullptr);
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

        QCOMPARE(video_renderer_choose_codec(false, false), 0);
        QVERIFY2(stateChangeLogCount() > firstStartLogCount,
                 "Choosing the same codec after stop must restart the selected video pipeline");
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }
};

QTEST_MAIN(VideoRendererLifecycleTest)
#include "VideoRendererLifecycleTest.moc"

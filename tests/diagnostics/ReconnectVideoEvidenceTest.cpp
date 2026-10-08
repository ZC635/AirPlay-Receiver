#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include "app/MainWindow.h"
#include "backend/FakeAirPlayReceiver.h"
#include "support/CollectingDiagnosticLogSink.h"
#include "diagnostics/VideoObservation.h"
#include "backend/VideoFrameBridge.h"
#include "app/VideoSurfaceWidget.h"
#if AIRPLAY_WITH_UXPLAY
#include "backend/UxPlayReceiver.h"
#include "backend/GstAppSinkFrameSource.h"
#include "platform/MdnsPublishing.h"
#include <gst/app/gstappsrc.h>
#include <QScopeGuard>
#endif

class ToggleSink final : public DiagnosticLogSink {
public:
    bool enabled = false;
    QVector<DiagnosticEvent> events;
    bool failWrites = false;
    bool isActive() const override { return enabled; }
    void record(DiagnosticEvent event) override { if (failWrites) throw 1; events.append(std::move(event)); }
};
class ControlledSource final : public AppSinkFrameSource {
public:
    std::optional<VideoFrameSample> sample;
    void setFrameAvailableCallback(std::function<void()>) override {}
    void start() override {}
    std::optional<VideoFrameSample> pullSample() override { return std::exchange(sample, {}); }
};
#if AIRPLAY_WITH_UXPLAY
class EvidencePublisher final : public MdnsPublishing {
public:
    bool publish(const QString &, const QByteArray &, quint16, const char *, int, const char *, int) override { return true; }
    void stop() override {}
};
#endif
class ReconnectVideoEvidenceTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_runtime;
private slots:
    void initTestCase() {
        const QString parent = qEnvironmentVariable("AIRPLAY_VIDEO_EVIDENCE_RUNTIME_PARENT", QDir::tempPath());
        QVERIFY(QDir(parent).exists());
        m_runtime = std::make_unique<QTemporaryDir>(QDir(parent).filePath("video-observation-XXXXXX"));
        QVERIFY(m_runtime->isValid());
#if AIRPLAY_WITH_UXPLAY
        qputenv("GST_REGISTRY_1_0", QDir(m_runtime->path()).filePath("registry.bin").toLocal8Bit());
#endif
    }

#if AIRPLAY_WITH_UXPLAY
    void actualReceiverBoundariesReportPendingSelectionAndExactCodecReturn() {
        CollectingSink sink;
        EvidencePublisher publisher;
        UxPlayReceiverConfig config;
        config.videoSink = "appsink";
        config.audioSink = "fakesink";
        config.mdnsPublisher = &publisher;
        config.diagnosticSink = &sink;
        UxPlayReceiver receiver(config);
        receiver.setVideoFrameCallback([](QImage) {});
        receiver.start();
        auto cleanup = qScopeGuard([&] { receiver.stop(); });
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        bool pendingStart = false;
        for (const auto &e : sink.events)
            if (e.name == "video_boundary" && e.fields.value("boundary") == "start_after") {
                QCOMPARE(e.fields.value("selected_pipeline"), QString("absent_selection_pending_or_unavailable"));
                QCOMPARE(e.fields.value("current_state"), QString("unknown"));
                QCOMPARE(e.fields.value("query_return"), QString("not_queried"));
                pendingStart = true;
            }
        QVERIFY(pendingStart);
        const auto generation = receiver.callbackGenerationForUxPlayCallback();
        receiver.handleConnectionInitializedFromUxPlayCallback(generation);
        QCOMPARE(receiver.chooseVideoCodecFromCallback(false, generation), 0);
        QCoreApplication::processEvents();
        bool exactReturn = false;
        bool newBinding = false;
        for (const auto &e : sink.events) {
            if (e.name != "video_boundary") continue;
            if (e.fields.value("boundary") == "codec_after") {
                QCOMPARE(e.fields.value("call_return"), QString("0"));
                QCOMPARE(e.fields.value("selected_pipeline"), QString("present"));
                QVERIFY(e.fields.value("query_return") != "not_queried");
                exactReturn = true;
            }
            if (e.fields.value("boundary") == "bridge_binding" && e.fields.value("binding") == "new") newBinding = true;
        }
        QVERIFY(exactReturn);
        QVERIFY(newBinding);
        receiver.stopVideoPipelineForDisconnect(generation);
        receiver.handleConnectionInitializedFromUxPlayCallback(generation);
        receiver.stopVideoPipelineForDisconnect(generation); // already stopped, zero-data failed cycle still closes
        const auto summary = sink.events.back();
        QCOMPARE(summary.name, QString("video_cycle_summary"));
        QCOMPARE(summary.fields.value("cycle_kind"), QString("connection"));
        QCOMPARE(summary.fields.value("input"), QString("0"));
    }
    void realRawAppsinkOutputIsObservedWithoutEncodingOrRecording() {
        gst_init(nullptr, nullptr);
        CollectingSink sink;
        auto context = std::make_shared<VideoObservation>(&sink);
        context->serviceBegin();
        context->connectionBegin();
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch("appsrc name=input caps=video/x-raw,format=RGBA,width=2,height=2,framerate=30/1 ! appsink name=output", &error);
        QVERIFY(error == nullptr);
        QVERIFY(pipeline);
        auto cleanup = qScopeGuard([&] { gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); });
        auto *input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
        auto *output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
        QVERIFY(input);
        QVERIFY(output);
        auto elementCleanup = qScopeGuard([&] { gst_object_unref(input); gst_object_unref(output); });
        GstAppSinkFrameSource source(output);
        source.setVideoObservation(context);
        VideoFrameBridge bridge(&source);
        bridge.setVideoObservation(context);
        std::atomic_int delivered = 0;
        connect(&bridge, &VideoFrameBridge::frameReady, &bridge, [&](QImage frame) {
            if (frame.size() == QSize(2, 2)) delivered.fetch_add(1);
        }, Qt::DirectConnection);
        bridge.start();
        QVERIFY(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
        for (int i = 0; i < 2; ++i) {
            GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 16, nullptr);
            QVERIFY(buffer);
            QCOMPARE(gst_app_src_push_buffer(GST_APP_SRC(input), buffer), GST_FLOW_OK);
        }
        QTRY_COMPARE(delivered.load(), 2);
        context->close("raw_only");
        QCOMPARE(countEvents(sink, "video_first_appsink"), 1);
        QCOMPARE(sink.events.back().fields.value("appsink"), QString("2"));
        QCOMPARE(sink.events.back().fields.value("bridge_accepted"), QString("2"));
        QCOMPARE(sink.events.back().fields.value("source_rejected"), QString("0"));
    }
#endif

    void inactiveAndFailedSinkDoNotQueryOrAccumulate() {
        ToggleSink sink;
        auto context = std::make_shared<VideoObservation>(&sink);
        int queries = 0;
        context->serviceBegin();
        context->connectionBegin();
        context->observe(VideoObservation::Input);
        context->boundary("test", [&] { ++queries; return QMap<QString, QString>{}; });
        context->close("disabled");
        QCOMPARE(queries, 0);
        QVERIFY(sink.events.isEmpty());
        sink.enabled = true;
        context->close("zero_after_disabled");
        QCOMPARE(sink.events.back().fields.value("input"), QString("0"));
        QCOMPARE(sink.events.back().fields.value("service_observation"), QString("0"));
        sink.enabled = false;
        context->boundary("failure_disabled", [&] { ++queries; return QMap<QString, QString>{}; });
        QCOMPARE(queries, 0);
        sink.enabled = true;
        sink.failWrites = true;
        context->observe(VideoObservation::Input); // sink write failure disables further probes locally
        context->boundary("write_failure", [&] { ++queries; return QMap<QString, QString>{}; });
        QCOMPARE(queries, 0);
        VideoObservation nullContext(nullptr);
        nullContext.boundary("null", [&] { ++queries; return QMap<QString, QString>{}; });
        QCOMPARE(queries, 0);
    }
    void boundedRealBridgeAndWidgetCountsResetAcrossCycles() {
        CollectingSink sink;
        auto context = std::make_shared<VideoObservation>(&sink);
        ControlledSource source;
        VideoFrameBridge bridge(&source);
        bridge.setVideoObservation(context);
        VideoSurfaceWidget widget;
        widget.resize(32, 32);
        widget.setVideoObservation(context);
        connect(&bridge, &VideoFrameBridge::frameReady, &widget, &VideoSurfaceWidget::onFrameReady, Qt::QueuedConnection);
        context->serviceBegin();
        context->connectionBegin();
        for (int i = 0; i < 5; ++i) {
            source.sample = VideoFrameSample{QByteArray(16, char(0x7f)), 2, 2, 8, "RGBA"};
            bridge.processFrame();
        }
        QTRY_COMPARE(countEvents(sink, "video_first_cache_commit"), 1);
        QImage target(32, 32, QImage::Format_RGBA8888);
        widget.render(&target);
        bridge.processFrame(); // no sample is rejected by the real bridge
        context->close("disconnect");
        const auto summary = findEvent(sink, "video_first_bridge_accepted");
        QCOMPARE(countEvents(sink, "video_first_bridge_accepted"), 1);
        QCOMPARE(countEvents(sink, "video_first_qt_received"), 1);
        const auto closed = sink.events.back();
        QCOMPARE(closed.fields.value("bridge_accepted"), QString("5"));
        QCOMPARE(closed.fields.value("bridge_rejected"), QString("1"));
        QCOMPARE(closed.fields.value("qt_received"), QString("5"));
        QCOMPARE(closed.fields.value("image_paint"), QString("1"));
        QCOMPARE(closed.fields.value("input"), QString("0"));
        QCOMPARE(closed.fields.value("cycle_observation"), summary.fields.value("cycle_observation"));
        QCOMPARE(closed.fields.value("attribution"), QString("observation_time_pixel_origin_unknown"));
        context->connectionBegin();
        context->close("zero_data_disconnect");
        QCOMPARE(sink.events.back().fields.value("bridge_accepted"), QString("0"));
        QVERIFY(sink.events.back().fields.value("cycle_observation") != closed.fields.value("cycle_observation"));
        context->serviceBegin();
        context->close("service_stop");
        QCOMPARE(sink.events.back().fields.value("service_observation"), QString("2"));
    }
    void queuedReceiptAfterDisconnectIsExplicitlyUnassigned() {
        CollectingSink sink;
        auto context = std::make_shared<VideoObservation>(&sink);
        VideoSurfaceWidget widget;
        widget.setVideoObservation(context);
        context->serviceBegin();
        context->connectionBegin();
        QImage frame(2, 2, QImage::Format_RGBA8888);
        frame.fill(Qt::red);
        QMetaObject::invokeMethod(&widget, [&] { widget.onFrameReady(frame); }, Qt::QueuedConnection);
        context->close("disconnect");
        QTRY_COMPARE(countEvents(sink, "video_first_qt_received"), 1);
        const auto event = findEvent(sink, "video_first_qt_received");
        QCOMPARE(event.fields.value("cycle_observation"), QString("0"));
        QCOMPARE(event.fields.value("cycle_kind"), QString("unassigned"));
        context->close("service_stop");
    }

    void realReceiverCallbackReachesQtCacheAndPaint() {
        CollectingSink sink;
        FakeAirPlayReceiver receiver;
        MainWindowRuntimeServices services;
        services.diagnosticSink = &sink;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, {}, nullptr, nullptr, services);
        window.show();
        QImage frame(8, 8, QImage::Format_RGBA8888);
        frame.fill(Qt::red);
        receiver.frameCallback()(frame);
        QTRY_VERIFY(hasEvent(sink, "video_first_qt_received"));
        QTRY_VERIFY(hasEvent(sink, "video_first_cache_commit"));
        QTRY_VERIFY(hasEvent(sink, "video_first_image_paint"));
    }
};
QTEST_MAIN(ReconnectVideoEvidenceTest)
#include "ReconnectVideoEvidenceTest.moc"

#include "backend/UxPlayVideoLifecycle.h"

#include "backend/GstAppSinkFrameSource.h"
#include "backend/VideoFrameBridge.h"
#include "backend/VideoQualitySettings.h"
#include "diagnostics/VideoObservation.h"
#include "lib/logger.h"
#include "renderers/video_renderer.h"
#include <utility>

namespace {
const char *videoApplicationName() {
    // GLib identity is process-wide; discovery names may change independently.
    const char *name = g_get_application_name();
    return name ? name : "AirPlay Receiver";
}
}

UxPlayVideoLifecycle::UxPlayVideoLifecycle(Options options, QObject *parent)
    : QObject(parent), m_options(std::move(options)) {}

UxPlayVideoLifecycle::~UxPlayVideoLifecycle() { shutdown(); }

bool UxPlayVideoLifecycle::initialize(void *logger, const QString &sink) {
    registerSampleTap();
    observeCall(QStringLiteral("video_renderer_init"));
    videoflip_t flip[2] = {NONE, NONE};
    const auto videoSink = sink.toUtf8();
    if (video_renderer_init(static_cast<logger_t *>(logger), videoApplicationName(), flip,
                            "h264parse", "", "decodebin", "videoconvert", videoSink.constData(),
                            "", false, false, videoQualityH265Support(), false, 3, nullptr) != 0)
        return false;
    m_initialized = true;
    return true;
}

void UxPlayVideoLifecycle::start() {
    video_renderer_start();
    observeBoundary("start_after");
    m_stopped.store(false);
    attachFrameBridge();
}

UxPlayVideoLifecycle::ResetResult UxPlayVideoLifecycle::reset(
    void *logger, const QString &sink, bool fitMode) {
    m_stopped.store(true);
    observeCall(QStringLiteral("video_renderer_stop"));
    observeBoundary("stop_before");
    video_renderer_stop();
    observeBoundary("stop_after");
    clearSampleTap();
    observeCall(QStringLiteral("video_frame_bridge_reset"));
    resetFrameBridge();
    observeCall(QStringLiteral("video_renderer_destroy"));
    video_renderer_destroy();
    m_initialized = false;

    if (!initialize(logger, sink)) {
        clearSampleTap();
        return ResetResult::InitializationFailed;
    }
    setFitMode(fitMode);
    observeCall(QStringLiteral("video_renderer_start"));
    video_renderer_start();
    observeBoundary("start_after");
    observeCall(m_h265 ? QStringLiteral("video_choose_h265") : QStringLiteral("video_choose_h264"));
    const int result = chooseCodec(m_h265);
    observeBoundary("reset_codec_after", result, true);
    if (result != 0) return ResetResult::CodecSelectionFailed;
    observeCall(QStringLiteral("video_frame_bridge_attach"));
    attachFrameBridge();
    m_stopped.store(false);
    return ResetResult::Ready;
}

bool UxPlayVideoLifecycle::stopForDisconnect() {
    if (m_stopped.exchange(true)) return false;
    observeBoundary("stop_before");
    video_renderer_stop();
    observeBoundary("stop_after");
    return true;
}

void UxPlayVideoLifecycle::resumeAfterDisconnect() {
    if (m_stopped.exchange(false)) {
        video_renderer_start();
        observeBoundary("start_after");
    }
}

void UxPlayVideoLifecycle::shutdown() {
    resetFrameBridge();
    clearSampleTap();
    if (m_initialized) {
        observeBoundary("stop_before");
        video_renderer_stop();
        observeBoundary("stop_after");
        observeCall(QStringLiteral("video_renderer_destroy"));
        video_renderer_destroy();
        m_initialized = false;
    }
    m_stopped.store(false);
}

void UxPlayVideoLifecycle::registerSampleTap() {
    if (m_tap.context) return;
    m_tap = m_options.createSampleTap();
    observeCall(QStringLiteral("video_set_sample_callback"));
    video_renderer_set_sample_callback(m_options.sampleCallback, m_tap.context);
}

void UxPlayVideoLifecycle::clearSampleTap() {
    if (!m_tap.context) return;
    observeCall(QStringLiteral("video_clear_sample_callback"));
    video_renderer_set_sample_callback(nullptr, nullptr);
    auto tap = std::exchange(m_tap, {});
    tap.release();
    observeCall(QStringLiteral("video_tap_context_closed"));
}

int UxPlayVideoLifecycle::chooseCodec(bool h265) const {
    return m_options.codecChooser ? m_options.codecChooser(false, h265)
                                  : video_renderer_choose_codec(false, h265);
}

int UxPlayVideoLifecycle::selectCodec(bool h265) {
    const int result = chooseCodec(h265);
    observeBoundary("codec_after", result, true);
    if (result == 0) m_h265 = h265;
    return result;
}

void UxPlayVideoLifecycle::attachSelectedCodec(bool h265) {
    if (!m_stopped.load() && m_initialized && m_h265 == h265) attachFrameBridge();
}

void UxPlayVideoLifecycle::setFitMode(bool enabled) {
    video_renderer_set_force_aspect_ratio(enabled);
}

void UxPlayVideoLifecycle::setFrameCallback(std::function<void(QImage)> callback) {
    m_frameCallback = std::move(callback);
    resetFrameBridge();
}

void UxPlayVideoLifecycle::resetFrameBridge() {
    delete m_bridge;
    m_bridge = nullptr;
    m_observedBridgePipeline = nullptr;
}

void UxPlayVideoLifecycle::attachFrameBridge() {
    const auto &observation = m_options.observation;
    if (m_bridge || !m_frameCallback) {
        observation->boundary("bridge_binding", [&] {
            QMap<QString, QString> fields{{"binding", m_bridge ? "reused" : "absent_no_callback"}};
            if (m_bridge) {
                void *selected = video_renderer_get_pipeline();
                fields.insert("selected_pipeline", selected ? "present" : "absent");
                fields.insert("matches_bound_pipeline", selected ? (selected == m_observedBridgePipeline ? "true" : "false") : "unknown");
            }
            return fields;
        });
        return;
    }
    auto *pipeline = static_cast<GstElement *>(video_renderer_get_pipeline());
    if (!pipeline) {
        observation->boundary("bridge_binding", [] { return QMap<QString, QString>{{"binding", "absent_no_selected_pipeline"}}; });
        return;
    }
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h264");
    if (!appsink) appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h265");
    if (!appsink) {
        observation->boundary("bridge_binding", [] { return QMap<QString, QString>{{"binding", "absent_no_appsink"}}; });
        return;
    }
    auto source = std::make_unique<GstAppSinkFrameSource>(appsink);
    source->setVideoObservation(observation);
    m_bridge = new VideoFrameBridge(std::move(source), this);
    m_bridge->setVideoObservation(observation);
    if (observation->active()) m_observedBridgePipeline = pipeline;
    observation->boundary("bridge_binding", [] { return QMap<QString, QString>{{"binding", "new"}, {"matches_bound_pipeline", "true"}}; });
    m_bridge->start();
    QObject::connect(m_bridge, &VideoFrameBridge::frameReady, this, [this](QImage frame) {
        m_options.observation->observe(VideoObservation::ReceiverQueued);
        if (m_frameCallback) m_frameCallback(frame);
    }, Qt::QueuedConnection);
    gst_object_unref(appsink);
}

void UxPlayVideoLifecycle::observeCall(const QString &call) const {
    if (m_options.callObserver) m_options.callObserver(call);
}

void UxPlayVideoLifecycle::observeBoundary(const char *name, int result, bool hasResult) const {
    m_options.observation->boundary(name, [&] {
        QMap<QString, QString> fields{{"call_return", hasResult ? QString::number(result) : QStringLiteral("void_no_return")},
                                     {"initialized", m_initialized ? "true" : "false"},
                                     {"stopped_flag", m_stopped.load() ? "true" : "false"}};
        auto *pipeline = static_cast<GstElement *>(video_renderer_get_pipeline());
        fields.insert("selected_pipeline", pipeline ? "present" : "absent_selection_pending_or_unavailable");
        if (pipeline) {
            GstState current = GST_STATE_VOID_PENDING, pending = GST_STATE_VOID_PENDING;
            const auto query = gst_element_get_state(pipeline, &current, &pending, 0);
            fields.insert("query_return", QString::number(query));
            fields.insert("current_state", QString::fromLatin1(gst_element_state_get_name(current)));
            fields.insert("pending_state", QString::fromLatin1(gst_element_state_get_name(pending)));
        } else {
            fields.insert("query_return", "not_queried");
            fields.insert("current_state", "unknown");
            fields.insert("pending_state", "unknown");
        }
        return fields;
    });
}

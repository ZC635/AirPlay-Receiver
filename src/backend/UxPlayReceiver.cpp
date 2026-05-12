#include "backend/UxPlayReceiver.h"
#include "backend/DiscoveryRestartController.h"
#include "backend/ReceiverStatePolicy.h"
#include "backend/VideoFrameBridge.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#if AIRPLAY_WITH_UXPLAY
#include "lib/dnssd.h"
#include "lib/logger.h"
#include "lib/raop.h"
#include "platform/MdnsPublisher.h"
#include <glib.h>
#include "renderers/audio_renderer.h"
#include "renderers/video_renderer.h"

namespace {
constexpr unsigned short kDynamicPort = 0;
constexpr bool kAudioSync = false;
constexpr bool kVideoSync = false;
constexpr unsigned int kPlaybinVersion = 3;

bool debugLogEnabled() {
    return !qgetenv("AIRPLAY_DEBUG_LOG").isEmpty();
}

void debugLog(const char *format, ...) {
    if (!debugLogEnabled()) {
        return;
    }

    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/airplay_receiver_debug.log");
    FILE *file = std::fopen(path.toLocal8Bit().constData(), "ab");
    if (!file) {
        return;
    }

    char message[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    const QByteArray timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8();
    std::fprintf(file, "%sZ %s\n", timestamp.constData(), message);
    std::fclose(file);
}

void logConnectionReset(const char *message) {
    qWarning().noquote() << message;
    debugLog("%s", message);
}

QByteArray defaultDeviceId() {
    return QByteArrayLiteral("02:00:00:00:00:01");
}

QByteArray defaultHardwareAddress() {
    return QByteArray::fromHex("020000000001");
}

double volumeFromAirPlayDb(float volume) {
    if (volume == -144.0F || volume <= -30.0F) {
        return 0.0;
    }
    if (volume >= 0.0F) {
        return 1.0;
    }
    return std::pow(10.0, 0.05 * volume);
}

void logCallback(void *, int level, const char *msg) {
    debugLog("uxplay[%d] %s", level, msg ? msg : "");
}

void audioProcess(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->renderAudioBufferFromCallback(data->data, &data->data_len, &data->seqnum, &data->ntp_time_remote);
}

void videoProcess(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->renderVideoBufferFromCallback(data->data, &data->data_len, &data->nal_count, &data->ntp_time_remote);
}

void audioFlush(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->flushAudioRendererFromCallback();
}

void videoFlush(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->flushVideoRendererFromCallback();
}

void videoPause(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->pauseVideoRendererFromCallback();
}

void videoResume(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->resumeVideoRendererFromCallback();
}

double audioSetClientVolume(void *cls) {
    const auto *receiver = static_cast<UxPlayReceiver *>(cls);
    Q_UNUSED(receiver);
    return 0.0;
}

void audioSetVolume(void *cls, float volume) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->setVolumeFromUxPlayCallback(volumeFromAirPlayDb(volume));
}

void audioGetFormat(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    Q_UNUSED(spf);
    Q_UNUSED(usingScreen);
    Q_UNUSED(isMedia);
    Q_UNUSED(audioFormat);
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->startAudioRendererFromUxPlayCallback(ct);
}

void videoReportSize(void *cls, float *widthSource, float *heightSource, float *width, float *height) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    int w = static_cast<int>(*widthSource);
    int h = static_cast<int>(*heightSource);
    *widthSource = 1920.0F;
    *heightSource = 1080.0F;
    *width = 1920.0F;
    *height = 1080.0F;
    if (w > 0 && h > 0) {
        QPointer<UxPlayReceiver> guardedReceiver(receiver);
        QMetaObject::invokeMethod(receiver, [guardedReceiver, w, h] {
            if (guardedReceiver) {
                emit guardedReceiver->videoSizeChanged(w, h);
            }
        }, Qt::QueuedConnection);
    }
}

void connInit(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    const auto generation = receiver->callbackGenerationForUxPlayCallback();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation] {
        if (guardedReceiver) {
            guardedReceiver->restartVideoPipelineForConnect();
            guardedReceiver->setStateFromUxPlayCallback(ReceiverState::Connected, generation);
        }
    }, Qt::QueuedConnection);
}

void connDestroy(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    debugLog("connDestroy callback fired");
    const auto generation = receiver->callbackGenerationForUxPlayCallback();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation] {
        if (guardedReceiver) {
            guardedReceiver->stopVideoPipelineForDisconnect();
            guardedReceiver->setStateFromUxPlayCallback(ReceiverState::Discoverable, generation);
        }
    }, Qt::QueuedConnection);
}

void connReset(void *, int reason) {
    switch (reason) {
    case 1:
        logConnectionReset("ERROR lost connection with client (network problem?)");
        break;
    case 2:
        logConnectionReset("ERROR Unsupported HLS streaming source");
        break;
    default:
        qWarning("ERROR connection reset (reason=%d)", reason);
        debugLog("ERROR connection reset (reason=%d)", reason);
        break;
    }
}

void connFeedback(void *) {
}

void videoReset(void *cls, reset_type_t type) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    const auto generation = receiver->callbackGenerationForUxPlayCallback();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation, type] {
        if (guardedReceiver) {
            guardedReceiver->handleVideoResetFromUxPlayCallback(static_cast<int>(type), generation);
        }
    }, Qt::QueuedConnection);
}

void audioSetMetadata(void *cls, const void *buffer, int buflen) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);

    if (buflen < 8) {
        return;
    }

    const unsigned char *metadata = static_cast<const unsigned char *>(buffer);
    char dmap_tag[5] = {0};
    int datalen = 0;

    for (int i = 0; i < 4; i++) {
        dmap_tag[i] = static_cast<char>(metadata[i]);
    }
    for (int i = 0; i < 4; i++) {
        datalen <<= 8;
        datalen += static_cast<int>(metadata[4 + i]);
    }

    if (std::strcmp(dmap_tag, "mlit") != 0 || datalen != buflen - 8) {
        return;
    }

    metadata += 8;
    buflen -= 8;

    QString title, artist, album;

    while (buflen >= 8) {
        char item_tag[5] = {0};
        int item_len = 0;

        for (int i = 0; i < 4; i++) {
            item_tag[i] = static_cast<char>(metadata[i]);
        }
        for (int i = 0; i < 4; i++) {
            item_len <<= 8;
            item_len += static_cast<int>(metadata[4 + i]);
        }

        metadata += 8;
        buflen -= 8;

        if (item_len <= 0 || item_len > buflen) {
            break;
        }

        QString value = QString::fromUtf8(reinterpret_cast<const char *>(metadata), item_len);

        if (std::strcmp(item_tag, "minm") == 0 || std::strcmp(item_tag, "\302\251nam") == 0) {
            title = value;
        } else if (std::strcmp(item_tag, "asar") == 0 || std::strcmp(item_tag, "\302\251ART") == 0) {
            artist = value;
        } else if (std::strcmp(item_tag, "asal") == 0 || std::strcmp(item_tag, "\302\251alb") == 0) {
            album = value;
        }

        metadata += item_len;
        buflen -= item_len;
    }

    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, title, artist, album] {
        if (guardedReceiver) {
            emit guardedReceiver->trackMetadataChanged(title, artist, album);
        }
    }, Qt::QueuedConnection);
}

void audioSetCoverart(void *cls, const void *buffer, int buflen) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->setCoverArtFromUxPlayCallback(buffer, buflen);
}

void audioStopCoverartRendering(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->stopCoverArtRenderingFromUxPlayCallback();
}

void audioSetProgress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);

    uint32_t s = *start;
    uint32_t c = *curr;
    uint32_t e = *end;

    if (c < s || c > e) {
        return;
    }

    int positionSec = static_cast<int>((c - s) / 44100);
    int durationSec = static_cast<int>((e - s) / 44100);

    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, positionSec, durationSec] {
        if (guardedReceiver) {
            emit guardedReceiver->progressUpdated(positionSec, durationSec);
        }
    }, Qt::QueuedConnection);
}

void reportClientRequest(void *, char *, char *, char *, bool *admit) {
    *admit = true;
}

int videoSetCodec(void *cls, video_codec_t codec) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    bool video_is_h265 = (codec == VIDEO_CODEC_H265);
    return receiver->chooseVideoCodecFromCallback(video_is_h265);
}
} // namespace
#endif

UxPlayReceiver::UxPlayReceiver(UxPlayReceiverConfig config, QObject *parent)
    : AirPlayReceiver(parent), m_config(std::move(config)) {
#if AIRPLAY_WITH_UXPLAY
    m_discoveryRestartController = new DiscoveryRestartController(2000, this);
#endif
}

UxPlayReceiver::~UxPlayReceiver() {
#if AIRPLAY_WITH_UXPLAY
    cleanupUxPlay();
#endif
}

void UxPlayReceiver::start() {
#if AIRPLAY_WITH_UXPLAY
    if (m_state != ReceiverState::Idle) {
        return;
    }

    setState(ReceiverState::Starting);
    m_callbackGeneration.fetch_add(1);
    m_acceptingCallbacks.store(true);

    const QString pluginDir = QCoreApplication::applicationDirPath() + "/gstreamer-plugins";
    if (QDir(pluginDir).exists()) {
        qputenv("GST_PLUGIN_PATH", pluginDir.toLocal8Bit());
    }

    if (!gstreamer_init()) {
        m_acceptingCallbacks.store(false);
        setError("Failed to initialize GStreamer");
        setState(ReceiverState::Error);
        return;
    }

    m_logger = logger_init();
    if (!m_logger) {
        m_acceptingCallbacks.store(false);
        setError("Failed to initialize UxPlay logger");
        setState(ReceiverState::Error);
        return;
    }
    auto *logger = static_cast<logger_t *>(m_logger);
    logger_set_callback(logger, logCallback, this);
    logger_set_level(logger, debugLogEnabled() ? LOGGER_DEBUG : LOGGER_INFO);

    const QByteArray serverName = m_config.serverName.toUtf8();
    const QByteArray videoSink = m_config.videoSink.toUtf8();
    const QByteArray audioSink = m_config.audioSink.toUtf8();
    const bool h265Support = videoQualityH265Support();
    videoflip_t videoFlip[2] = {NONE, NONE};
    if (video_renderer_init(logger, serverName.constData(), videoFlip, "h264parse", "",
                             "decodebin", "videoconvert", videoSink.constData(), "", false, kVideoSync, h265Support, false,
                             kPlaybinVersion, nullptr) != 0) {
        m_acceptingCallbacks.store(false);
        setError("Failed to initialize GStreamer video renderer");
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }
    video_renderer_start();
    m_videoRendererStopped.store(false);
    attachVideoFrameBridgeToCurrentPipeline();
    if (audio_renderer_init(logger, audioSink.constData(), &kAudioSync, &kVideoSync, "") != 0) {
        m_acceptingCallbacks.store(false);
        setError("Failed to initialize GStreamer audio renderer");
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }
    m_renderersStarted.store(true);

    m_glibTimer = new QTimer();
    QObject::connect(static_cast<QTimer *>(m_glibTimer), &QTimer::timeout, [] {
        g_main_context_iteration(g_main_context_default(), FALSE);
    });
    static_cast<QTimer *>(m_glibTimer)->start(16);

    raop_callbacks_t callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.cls = this;
    callbacks.audio_process = audioProcess;
    callbacks.video_process = videoProcess;
    callbacks.video_pause = videoPause;
    callbacks.video_resume = videoResume;
    callbacks.conn_feedback = connFeedback;
    callbacks.conn_reset = connReset;
    callbacks.video_reset = videoReset;
    callbacks.conn_init = connInit;
    callbacks.conn_destroy = connDestroy;
    callbacks.audio_flush = audioFlush;
    callbacks.video_flush = videoFlush;
    callbacks.audio_set_client_volume = audioSetClientVolume;
    callbacks.audio_set_volume = audioSetVolume;
    callbacks.audio_set_metadata = audioSetMetadata;
    callbacks.audio_set_coverart = audioSetCoverart;
    callbacks.audio_stop_coverart_rendering = audioStopCoverartRendering;
    callbacks.audio_set_progress = audioSetProgress;
    callbacks.audio_get_format = audioGetFormat;
    callbacks.video_report_size = videoReportSize;
    callbacks.report_client_request = reportClientRequest;
    callbacks.video_set_codec = videoSetCodec;

    auto *raop = raop_init(&callbacks);
    if (!raop) {
        setError("Failed to initialize RAOP");
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }
    raop_set_log_callback(raop, logCallback, this);
    raop_set_log_level(raop, debugLogEnabled() ? LOGGER_DEBUG : LOGGER_INFO);
    m_raop = raop;

    const QByteArray deviceId = defaultDeviceId();
    if (raop_init2(raop, 0, deviceId.constData(), "") != 0) {
        setError("Failed to initialize RAOP pairing");
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }
    m_raopHttpdInitialized = true;

    unsigned short port = static_cast<unsigned short>(m_config.basePort > 0 ? m_config.basePort : kDynamicPort);
    raop_set_port(raop, port);
    m_raopPort = port;

    raop_set_plist(raop, "width", videoQualityWidth(m_config.videoQuality.resolution));
    raop_set_plist(raop, "height", videoQualityHeight(m_config.videoQuality.resolution));
    raop_set_plist(raop, "refreshRate", videoQualityRefreshRate(m_config.videoQuality.frameRate));
    raop_set_plist(raop, "maxFPS", videoQualityMaxFPS(m_config.videoQuality.frameRate));

    if (!createDiscoveryBroadcast()) {
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }

    if (raop_start_httpd(raop, &port) < 0) {
        setError("Failed to start RAOP HTTP server");
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }
    m_raopHttpdStarted = true;
    raop_set_port(raop, port);
    m_raopPort = port;

    if (!registerDiscoveryBroadcast(m_raopPort)) {
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }

    setState(ReceiverState::Discoverable);
#else
    setError("UxPlay support is not enabled in this build");
    setState(ReceiverState::Error);
#endif
}

void UxPlayReceiver::stop() {
#if AIRPLAY_WITH_UXPLAY
    cleanupUxPlay();
#endif
    if (m_state != ReceiverState::Idle) {
        setState(ReceiverState::Idle);
    }
}

void UxPlayReceiver::setVolume(double volume) {
    m_volume.store(volume);
#if AIRPLAY_WITH_UXPLAY
    if (m_audioRendererStarted.load()) {
        audio_renderer_set_volume(volume);
    }
#endif
}

void UxPlayReceiver::setVideoSurface(WId id) {
    Q_UNUSED(id);
}

void UxPlayReceiver::setVideoFrameCallback(FrameCallback callback) {
    m_frameCallback = std::move(callback);
#if AIRPLAY_WITH_UXPLAY
    if (m_videoFrameBridge) {
        resetVideoFrameBridge();
    }
#endif
}

void UxPlayReceiver::setVideoFitMode(bool enabled) {
    m_videoFitMode.store(enabled);
#if AIRPLAY_WITH_UXPLAY
    applyVideoFitModeToRenderer();
#endif
}

ReceiverState UxPlayReceiver::state() const {
    return m_state;
}

QString UxPlayReceiver::receiverName() const {
    return m_config.serverName;
}

bool UxPlayReceiver::applyReceiverName(const QString &name) {
    const auto result = decideReceiverNameChange(m_state, m_config.serverName, name);

    switch (result.action) {
    case ReceiverPolicyAction::Reject: {
        debugLog("applyReceiverName: rejected %s", name.trimmed().isEmpty() ? "empty name" : "name");
        return false;
    }
    case ReceiverPolicyAction::Noop: {
        debugLog("applyReceiverName: name unchanged \"%s\"", qPrintable(m_config.serverName));
        return true;
    }
    case ReceiverPolicyAction::StoreOnly: {
        m_config.serverName = result.normalizedName;
        debugLog("applyReceiverName: Idle state, name stored, no restart needed");
        return true;
    }
    case ReceiverPolicyAction::RestartDiscoveryConnected: {
        m_config.serverName = result.normalizedName;
        debugLog("applyReceiverName: set config name to \"%s\", state=%d", qPrintable(result.normalizedName), static_cast<int>(m_state));

#if AIRPLAY_WITH_UXPLAY
        debugLog("applyReceiverName: Connected/Connecting state, restarting discovery broadcast");
        if (!restartDiscoveryBroadcast()) {
            debugLog("applyReceiverName: restartDiscoveryBroadcast() failed, reverting name");
            m_config.serverName = result.rollbackName;
            return false;
        }
        debugLog("applyReceiverName: restartDiscoveryBroadcast() succeeded in Connected state");
#else
        debugLog("applyReceiverName: non-UXPlay path, returning true");
#endif
        return true;
    }
    case ReceiverPolicyAction::RestartDiscovery: {
        m_config.serverName = result.normalizedName;
        debugLog("applyReceiverName: set config name to \"%s\", state=%d", qPrintable(result.normalizedName), static_cast<int>(m_state));

#if AIRPLAY_WITH_UXPLAY
        if (!restartDiscoveryBroadcast(result.rollbackName)) {
            debugLog("applyReceiverName: restartDiscoveryBroadcast() failed, reverting name");
            m_config.serverName = result.rollbackName;
            if (!restartDiscoveryBroadcast()) {
                debugLog("applyReceiverName: recovery restartDiscoveryBroadcast() also failed");
                setError("Failed to recover discovery after receiver rename failure");
                setState(ReceiverState::Error);
            }
            return false;
        }
        debugLog("applyReceiverName: restartDiscoveryBroadcast() succeeded");
#else
        debugLog("applyReceiverName: non-UXPlay path, returning true");
#endif
        return true;
    }
    case ReceiverPolicyAction::RestartReceiver:
        debugLog("applyReceiverName: unexpected RestartReceiver for name change");
        return true;
    }

    return false;
}

bool UxPlayReceiver::applyVideoQuality(const VideoQualitySettings &quality) {
    const auto result = decideVideoQualityChange(m_state, m_config.videoQuality, quality);

    switch (result.action) {
    case ReceiverPolicyAction::Reject: {
        debugLog("applyVideoQuality: rejected in %s state",
                 m_state == ReceiverState::Error ? "Error" : "Starting");
        return false;
    }
    case ReceiverPolicyAction::Noop: {
        debugLog("applyVideoQuality: quality unchanged");
        return true;
    }
    case ReceiverPolicyAction::StoreOnly: {
        m_config.videoQuality = quality;
        debugLog("applyVideoQuality: Idle state, quality stored");
        return true;
    }
    case ReceiverPolicyAction::RestartDiscovery: {
        const VideoQualitySettings previousQuality = m_config.videoQuality;
        m_config.videoQuality = quality;

#if AIRPLAY_WITH_UXPLAY
        if (m_raop) {
            auto *raop = static_cast<raop_t *>(m_raop);
            raop_set_plist(raop, "width", videoQualityWidth(quality.resolution));
            raop_set_plist(raop, "height", videoQualityHeight(quality.resolution));
            raop_set_plist(raop, "refreshRate", videoQualityRefreshRate(quality.frameRate));
            raop_set_plist(raop, "maxFPS", videoQualityMaxFPS(quality.frameRate));
        }
        if (!restartDiscoveryBroadcast()) {
            debugLog("applyVideoQuality: restartDiscoveryBroadcast() failed, reverting quality");
            m_config.videoQuality = previousQuality;
            return false;
        }
        debugLog("applyVideoQuality: Discoverable state, discovery restart scheduled");
#else
        debugLog("applyVideoQuality: non-UXPlay path, returning true");
#endif
        return true;
    }
    case ReceiverPolicyAction::RestartReceiver: {
        const VideoQualitySettings previousQuality = m_config.videoQuality;
        m_config.videoQuality = quality;

        debugLog("applyVideoQuality: Connected/Connecting state, restarting receiver");
        stop();
        start();
        if (m_state == ReceiverState::Error) {
            debugLog("applyVideoQuality: restart failed, reverting quality");
            m_config.videoQuality = previousQuality;
            return false;
        }
        debugLog("applyVideoQuality: receiver restarted successfully");
        return true;
    }
    }

    debugLog("applyVideoQuality: unexpected state, storing quality and returning true");
    return true;
}

#if AIRPLAY_WITH_UXPLAY
void UxPlayReceiver::setStateFromUxPlayCallback(ReceiverState state) {
    setStateFromUxPlayCallback(state, m_callbackGeneration.load());
}

void UxPlayReceiver::setStateFromUxPlayCallback(ReceiverState state, quint64 generation) {
    if (!m_acceptingCallbacks.load() || generation != m_callbackGeneration.load()) {
        return;
    }
    setState(state);
}

quint64 UxPlayReceiver::callbackGenerationForUxPlayCallback() const {
    return m_callbackGeneration.load();
}

void UxPlayReceiver::startAudioRendererFromUxPlayCallback(unsigned char *compressionType) {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load()) {
        return;
    }
    audio_renderer_start(compressionType);
    m_audioRendererStarted.store(true);
    audio_renderer_set_volume(m_volume.load());
}

void UxPlayReceiver::setVolumeFromUxPlayCallback(double volume) {
    m_volume.store(volume);
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_audioRendererStarted.load()) {
        return;
    }
    audio_renderer_set_volume(volume);
}

void UxPlayReceiver::setCoverArtFromUxPlayCallback(const void *buffer, int buflen) {
    if (!m_acceptingCallbacks.load() || !buffer || buflen <= 0) {
        return;
    }

    QByteArray data(static_cast<const char *>(buffer), buflen);
    QPointer<UxPlayReceiver> guardedReceiver(this);
    QMetaObject::invokeMethod(this, [guardedReceiver, data] {
        if (guardedReceiver) {
            emit guardedReceiver->coverArtReceived(data);
        }
    }, Qt::QueuedConnection);
}

void UxPlayReceiver::stopCoverArtRenderingFromUxPlayCallback() {
    // Cover art is surfaced through Qt, not rendered by the GStreamer video sink.
}

void UxPlayReceiver::handleVideoResetFromUxPlayCallback(int resetType) {
    handleVideoResetFromUxPlayCallback(resetType, m_callbackGeneration.load());
}

void UxPlayReceiver::handleVideoResetFromUxPlayCallback(int resetType, quint64 generation) {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || generation != m_callbackGeneration.load() || !m_renderersStarted.load()
        || m_state != ReceiverState::Connected) {
        return;
    }

    const auto restartVideoRenderer = [this] {
        m_videoRendererStopped.store(true);
        video_renderer_stop();
        resetVideoFrameBridge();
        video_renderer_destroy();

        auto *logger = static_cast<logger_t *>(m_logger);
        const QByteArray serverName = m_config.serverName.toUtf8();
        const QByteArray videoSink = m_config.videoSink.toUtf8();
        const bool h265Support = videoQualityH265Support();
        videoflip_t videoFlip[2] = {NONE, NONE};
        video_renderer_init(logger, serverName.constData(), videoFlip, "h264parse", "",
                            "decodebin", "videoconvert", videoSink.constData(), "", false, kVideoSync, h265Support, false,
                            kPlaybinVersion, nullptr);
        applyVideoFitModeToRenderer();
        video_renderer_start();
        if (video_renderer_choose_codec(false, false) == 0) {
            attachVideoFrameBridgeToCurrentPipeline();
        }
        m_videoRendererStopped.store(false);
    };

    switch (static_cast<reset_type_t>(resetType)) {
    case RESET_TYPE_NOHOLD:
    case RESET_TYPE_RTP_SHUTDOWN:
    case RESET_TYPE_HLS_SHUTDOWN:
    case RESET_TYPE_HLS_EOS:
    case RESET_TYPE_RTP_TO_HLS_TEARDOWN:
        restartVideoRenderer();
        break;
    case RESET_TYPE_ON_VIDEO_PLAY:
        break;
    }
}

void UxPlayReceiver::stopVideoPipelineForDisconnect() {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    if (m_videoRendererStopped.exchange(true)) {
        return;
    }
    video_renderer_stop();
}

void UxPlayReceiver::restartVideoPipelineForConnect() {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    if (m_videoRendererStopped.exchange(false)) {
        video_renderer_start();
    }
}

void UxPlayReceiver::applyVideoFitModeToRenderer() {
    video_renderer_set_force_aspect_ratio(m_videoFitMode.load());
}

void UxPlayReceiver::renderAudioBufferFromCallback(void *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time) {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    audio_renderer_render_buffer(static_cast<unsigned char *>(data), data_len, seqnum, ntp_time);
}

void UxPlayReceiver::renderVideoBufferFromCallback(void *data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    video_renderer_render_buffer(static_cast<unsigned char *>(data), data_len, nal_count, ntp_time);
}

void UxPlayReceiver::flushAudioRendererFromCallback() {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    audio_renderer_flush();
}

void UxPlayReceiver::flushVideoRendererFromCallback() {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    video_renderer_flush();
}

void UxPlayReceiver::pauseVideoRendererFromCallback() {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    video_renderer_pause();
}

void UxPlayReceiver::resumeVideoRendererFromCallback() {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return;
    }
    video_renderer_resume();
}

int UxPlayReceiver::chooseVideoCodecFromCallback(bool video_is_h265) {
    QMutexLocker locker(&m_rendererMutex);
    if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
        return -1;
    }
    int result = video_renderer_choose_codec(false, video_is_h265);
    if (result == 0) {
        attachVideoFrameBridgeToCurrentPipeline();
    }
    return result;
}

void UxPlayReceiver::resetVideoFrameBridge() {
    if (!m_videoFrameBridge) {
        return;
    }

    delete m_videoFrameBridge;
    m_videoFrameBridge = nullptr;
}

void UxPlayReceiver::attachVideoFrameBridgeToCurrentPipeline() {
    if (m_videoFrameBridge || !m_frameCallback) {
        return;
    }

    GstElement *pipeline = static_cast<GstElement *>(video_renderer_get_pipeline());
    if (!pipeline) {
        return;
    }

    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h264");
    if (!appsink) {
        appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h265");
    }
    if (!appsink) {
        return;
    }

    m_videoFrameBridge = new VideoFrameBridge(appsink, this);
    m_videoFrameBridge->start();
    QObject::connect(m_videoFrameBridge, &VideoFrameBridge::frameReady,
                     this, [this](QImage frame) {
                         if (m_frameCallback) m_frameCallback(frame);
                     }, Qt::QueuedConnection);
    gst_object_unref(appsink);
}
#endif

void UxPlayReceiver::setState(ReceiverState state) {
    if (m_state == state) {
        return;
    }

    m_state = state;
    emit stateChanged(m_state);
}

void UxPlayReceiver::setError(QString error) {
    if (m_error == error) {
        return;
    }

    m_error = std::move(error);
    emit errorChanged(m_error);
}

#if AIRPLAY_WITH_UXPLAY
bool UxPlayReceiver::createDiscoveryBroadcast() {
    if (!m_raop) {
        setError("Cannot initialize DNS-SD before RAOP initialization");
        return false;
    }
    if (m_dnssd) {
        return true;
    }

    int dnssdError = 0;
    const QByteArray serverName = m_config.serverName.toUtf8();
    const QByteArray hwAddress = defaultHardwareAddress();
    debugLog("createDiscoveryBroadcast: name=\"%s\", port=%u", serverName.constData(), m_raopPort);
    auto *dnssd = dnssd_init(serverName.constData(), serverName.size(), hwAddress.constData(), hwAddress.size(), &dnssdError, 0);
    if (dnssdError || !dnssd) {
        debugLog("createDiscoveryBroadcast: dnssd_init failed, error=%d", dnssdError);
        setError(QString("Failed to initialize DNS-SD: %1").arg(dnssdError));
        return false;
    }

    m_dnssd = dnssd;
    raop_set_dnssd(static_cast<raop_t *>(m_raop), dnssd);
    const bool h265Support = videoQualityH265Support();
    dnssd_set_airplay_features(dnssd, 42, h265Support ? 1 : 0);
    debugLog("createDiscoveryBroadcast: success");
    return true;
}

bool UxPlayReceiver::registerDiscoveryBroadcast(unsigned short port) {
    if (!m_dnssd) {
        debugLog("registerDiscoveryBroadcast: no DNS-SD object");
        setError("Cannot register DNS-SD services before DNS-SD initialization");
        return false;
    }

    debugLog("registerDiscoveryBroadcast: port=%u", port);
    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    int registerError = dnssd_register_raop(dnssd, port);
    if (registerError == 0) {
        registerError = dnssd_register_airplay(dnssd, port);
    }
    if (registerError != 0) {
        debugLog("registerDiscoveryBroadcast: failed, error=%d", registerError);
        setError(QString("Failed to register DNS-SD services: %1").arg(registerError));
        return false;
    }

    int raopTxtLength = 0;
    const char *raopTxt = dnssd_get_raop_txt(dnssd, &raopTxtLength);
    int airplayTxtLength = 0;
    const char *airplayTxt = dnssd_get_airplay_txt(dnssd, &airplayTxtLength);

    if (!m_mdnsPublisher) {
        m_mdnsPublisher = new MdnsPublisher(this);
    }
    const QByteArray hwAddress = defaultHardwareAddress();
    if (!m_mdnsPublisher->publish(m_config.serverName, hwAddress, port,
                                  raopTxt, raopTxtLength,
                                  airplayTxt, airplayTxtLength)) {
        debugLog("registerDiscoveryBroadcast: MdnsPublisher::publish failed");
    }

    debugLog("registerDiscoveryBroadcast: success");
    return true;
}

void UxPlayReceiver::stopDiscoveryBroadcast() {
    if (m_mdnsPublisher) {
        m_mdnsPublisher->stop();
    }

    if (!m_dnssd) {
        return;
    }

    debugLog("stopDiscoveryBroadcast");
    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    dnssd_destroy(dnssd);
    m_dnssd = nullptr;
}

void UxPlayReceiver::unregisterDiscoveryBroadcast() {
    if (m_mdnsPublisher) {
        m_mdnsPublisher->stop();
    }

    if (!m_dnssd) {
        return;
    }

    debugLog("unregisterDiscoveryBroadcast (Goodbye sent, handle kept alive)");
    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
}

void UxPlayReceiver::destroyDiscoveryBroadcast() {
    if (m_mdnsPublisher) {
        m_mdnsPublisher->stop();
        delete m_mdnsPublisher;
        m_mdnsPublisher = nullptr;
    }

    if (!m_dnssd) {
        return;
    }

    debugLog("destroyDiscoveryBroadcast");
    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_destroy(dnssd);
    m_dnssd = nullptr;
}

bool UxPlayReceiver::restartDiscoveryBroadcast(const QString &recoveryName) {
    const bool shouldStartHttpd = m_raopHttpdStarted || m_state == ReceiverState::Discoverable;

    DiscoveryRestartOperations ops;
    const auto guarded = QPointer<UxPlayReceiver>(this);

    ops.canRestart = [this] {
        return m_raop != nullptr && m_raopPort != 0;
    };
    ops.stopHttpdIfStarted = [this] {
        if (m_raop && m_raopHttpdStarted) {
            raop_stop_httpd(static_cast<raop_t *>(m_raop));
            m_raopHttpdStarted = false;
        }
    };
    ops.unregisterBroadcast = [this] {
        unregisterDiscoveryBroadcast();
    };
    ops.destroyBroadcast = [this, guarded] {
        if (!guarded) return;
        destroyDiscoveryBroadcast();
    };
    ops.createBroadcast = [this, guarded] {
        if (!guarded) return false;
        if (!m_raop) return false;
        return createDiscoveryBroadcast();
    };
    ops.startHttpdIfNeeded = [this, guarded] {
        if (!guarded) return false;
        if (!m_raop) return false;
        unsigned short port = m_raopPort;
        if (raop_start_httpd(static_cast<raop_t *>(m_raop), &port) < 0) {
            return false;
        }
        raop_set_port(static_cast<raop_t *>(m_raop), port);
        m_raopPort = port;
        m_raopHttpdStarted = true;
        return true;
    };
    ops.registerBroadcast = [this, guarded] {
        if (!guarded) return false;
        if (!m_raop) return false;
        return registerDiscoveryBroadcast(m_raopPort);
    };
    ops.restoreReceiverName = [this](QString name) {
        m_config.serverName = std::move(name);
    };
    ops.fail = [this](QString msg) {
        setError(std::move(msg));
        setState(ReceiverState::Error);
    };
    ops.canContinue = [guarded, this] {
        return guarded && m_acceptingCallbacks.load();
    };

    const bool scheduled = m_discoveryRestartController->schedule(recoveryName, shouldStartHttpd, std::move(ops));
    if (!scheduled) {
        stopDiscoveryBroadcast();
    }
    return scheduled;
}

void UxPlayReceiver::cleanupUxPlay() {
    m_acceptingCallbacks.store(false);
    m_callbackGeneration.fetch_add(1);

    m_discoveryRestartController->cancel();

    if (m_glibTimer) {
        static_cast<QTimer *>(m_glibTimer)->stop();
        delete static_cast<QTimer *>(m_glibTimer);
        m_glibTimer = nullptr;
    }

    if (m_raop && m_raopHttpdStarted) {
        raop_stop_httpd(static_cast<raop_t *>(m_raop));
        m_raopHttpdStarted = false;
    }
    stopDiscoveryBroadcast();
    m_raopPort = 0;
    if (m_raop) {
        raop_destroy(static_cast<raop_t *>(m_raop));
        m_raop = nullptr;
        m_raopHttpdInitialized = false;
    }
    if (m_videoFrameBridge) {
        delete m_videoFrameBridge;
        m_videoFrameBridge = nullptr;
    }
    if (m_renderersStarted.load()) {
        QMutexLocker rendererLocker(&m_rendererMutex);
        video_renderer_stop();
        video_renderer_destroy();
        audio_renderer_destroy();
        m_renderersStarted.store(false);
        m_audioRendererStarted.store(false);
        m_videoRendererStopped.store(false);
    }
    if (m_logger) {
        logger_destroy(static_cast<logger_t *>(m_logger));
        m_logger = nullptr;
    }
}
#endif

#include "backend/UxPlayReceiver.h"
#include "backend/ReceiverConfigurationChange.h"
#include "backend/ReceiverStatePolicy.h"
#include "backend/UxPlayDiscovery.h"
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

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#if AIRPLAY_WITH_UXPLAY
#include "lib/logger.h"
#include "lib/raop.h"
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

double volumeFromAirPlayDb(float volume) {
    if (volume == -144.0F || volume <= -30.0F) {
        return 0.0;
    }
    if (volume >= 0.0F) {
        return 1.0;
    }
    return std::pow(10.0, 0.05 * volume);
}

double volumeToAirPlayDb(double volume) {
    if (volume <= 0.0) {
        return -144.0;
    }
    return std::clamp(20.0 * std::log10(volume), -30.0, 0.0);
}

class CallbackScope {
public:
    explicit CallbackScope(void *cls) : m_context(static_cast<UxPlayReceiver::CallbackContext *>(cls)) {
        if (m_context) {
            m_entered = m_context->enter(&m_receiver);
        }
    }

    ~CallbackScope() {
        if (m_entered) {
            m_context->leave();
        }
    }

    UxPlayReceiver *receiver() const { return m_receiver; }
    quint64 generation() const { return m_context ? m_context->generation : 0; }

private:
    UxPlayReceiver::CallbackContext *m_context = nullptr;
    UxPlayReceiver *m_receiver = nullptr;
    bool m_entered = false;
};

void logCallback(void *cls, int level, const char *msg) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->handleLogMessageFromUxPlayCallback(level, msg, callback.generation());
    }
    debugLog("uxplay[%d] %s", level, msg ? msg : "");
}

void audioProcess(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->renderAudioBufferFromCallback(data->data, &data->data_len, &data->seqnum, &data->ntp_time_remote,
                                                callback.generation());
    }
}

void videoProcess(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->renderVideoBufferFromCallback(data->data, &data->data_len, &data->nal_count, &data->ntp_time_remote,
                                                callback.generation());
    }
}

void audioFlush(void *cls) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->flushAudioRendererFromCallback(callback.generation());
    }
}

void videoFlush(void *cls) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->flushVideoRendererFromCallback(callback.generation());
    }
}

void videoPause(void *cls) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->pauseVideoRendererFromCallback(callback.generation());
    }
}

void videoResume(void *cls) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->resumeVideoRendererFromCallback(callback.generation());
    }
}

double audioSetClientVolume(void *cls) {
    CallbackScope callback(cls);
    const auto *receiver = callback.receiver();
    return receiver ? receiver->currentVolumeForUxPlayClientVolumeCallback() : 0.0;
}

void audioSetVolume(void *cls, float volume) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->setVolumeFromUxPlayCallback(volumeFromAirPlayDb(volume), callback.generation());
    }
}

void audioGetFormat(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    Q_UNUSED(spf);
    Q_UNUSED(usingScreen);
    Q_UNUSED(isMedia);
    Q_UNUSED(audioFormat);
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->startAudioRendererFromUxPlayCallback(ct, callback.generation());
    }
}

void videoReportSize(void *cls, float *widthSource, float *heightSource, float *width, float *height) {
    int w = static_cast<int>(*widthSource);
    int h = static_cast<int>(*heightSource);
    *widthSource = 1920.0F;
    *heightSource = 1080.0F;
    *width = 1920.0F;
    *height = 1080.0F;
    if (w > 0 && h > 0) {
        CallbackScope callback(cls);
        if (auto *receiver = callback.receiver()) {
            receiver->reportVideoSizeFromUxPlayCallback(w, h, callback.generation());
        }
    }
}

void connInit(void *cls) {
    CallbackScope callback(cls);
    auto *receiver = callback.receiver();
    if (!receiver) return;
    const auto generation = callback.generation();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation] {
        if (guardedReceiver) {
            guardedReceiver->restartVideoPipelineForConnect(generation);
            guardedReceiver->setStateFromUxPlayCallback(ReceiverState::Connected, generation);
        }
    }, Qt::QueuedConnection);
}

void connDestroy(void *cls) {
    CallbackScope callback(cls);
    auto *receiver = callback.receiver();
    if (!receiver) return;
    debugLog("connDestroy callback fired");
    const auto generation = callback.generation();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation] {
        if (guardedReceiver) {
            guardedReceiver->stopVideoPipelineForDisconnect(generation);
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
    CallbackScope callback(cls);
    auto *receiver = callback.receiver();
    if (!receiver) return;
    const auto generation = callback.generation();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation, type] {
        if (guardedReceiver) {
            guardedReceiver->handleVideoResetFromUxPlayCallback(static_cast<int>(type), generation);
        }
    }, Qt::QueuedConnection);
}

void audioSetMetadata(void *cls, const void *buffer, int buflen) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->setMetadataFromUxPlayCallback(buffer, buflen, callback.generation());
    }
}

void audioSetCoverart(void *cls, const void *buffer, int buflen) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->setCoverArtFromUxPlayCallback(buffer, buflen, callback.generation());
    }
}

void audioStopCoverartRendering(void *cls) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->stopCoverArtRenderingFromUxPlayCallback();
    }
}

void audioSetProgress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    CallbackScope callback(cls);
    if (auto *receiver = callback.receiver()) {
        receiver->setProgressFromUxPlayCallback(*start, *curr, *end, callback.generation());
    }
}

void reportClientRequest(void *, char *, char *, char *, bool *admit) {
    *admit = true;
}

int videoSetCodec(void *cls, video_codec_t codec) {
    CallbackScope callback(cls);
    auto *receiver = callback.receiver();
    if (!receiver) return -1;
    bool video_is_h265 = (codec == VIDEO_CODEC_H265);
    return receiver->chooseVideoCodecFromCallback(video_is_h265, callback.generation());
}
} // namespace
#endif

UxPlayReceiver::UxPlayReceiver(UxPlayReceiverConfig config, QObject *parent)
    : AirPlayReceiver(parent),
      m_config(std::move(config))
#if AIRPLAY_WITH_UXPLAY
      , m_callbackDispatch(m_acceptingCallbacks, m_callbackGeneration, m_renderersStarted, m_rendererMutex)
#endif
{
#if AIRPLAY_WITH_UXPLAY
    UxPlayDiscoveryConfig discoveryConfig;
    discoveryConfig.receiverName = m_config.serverName;
    discoveryConfig.videoQuality = m_config.videoQuality;
    discoveryConfig.mdnsPublisher = m_config.mdnsPublisher;
    m_discovery = new UxPlayDiscovery(std::move(discoveryConfig), this);
    QObject::connect(m_discovery, &UxPlayDiscovery::failed, this, [this](const QString &message) {
        setError(message);
        setState(ReceiverState::Error);
    });
#endif
}

UxPlayReceiver::~UxPlayReceiver() {
#if AIRPLAY_WITH_UXPLAY
    cleanupUxPlay();
#endif
}

UxPlayReceiver::CallbackContext::CallbackContext(UxPlayReceiver *receiver, quint64 startGeneration)
    : generation(startGeneration), m_receiver(receiver) {
}

bool UxPlayReceiver::CallbackContext::enter(UxPlayReceiver **receiverOut) {
    QMutexLocker locker(&m_mutex);
    if (m_closed || !m_receiver) {
        if (receiverOut) {
            *receiverOut = nullptr;
        }
        return false;
    }
    ++m_inFlight;
    if (receiverOut) {
        *receiverOut = m_receiver;
    }
    return true;
}

void UxPlayReceiver::CallbackContext::leave() {
    QMutexLocker locker(&m_mutex);
    --m_inFlight;
    if (m_inFlight == 0) {
        m_idle.wakeAll();
    }
}

void UxPlayReceiver::CallbackContext::close() {
    QMutexLocker locker(&m_mutex);
    m_closed = true;
    m_receiver = nullptr;
    while (m_inFlight > 0) {
        m_idle.wait(&m_mutex);
    }
}

void UxPlayReceiver::start() {
#if AIRPLAY_WITH_UXPLAY
    if (m_state != ReceiverState::Idle) {
        return;
    }

    setState(ReceiverState::Starting);
    const auto generation = m_callbackGeneration.fetch_add(1) + 1;
    m_callbackContexts.push_back(std::make_unique<CallbackContext>(this, generation));
    m_callbackContext = m_callbackContexts.back().get();
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
    logger_set_callback(logger, logCallback, m_callbackContext);
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
    callbacks.cls = m_callbackContext;
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
    raop_set_log_callback(raop, logCallback, m_callbackContext);
    // UxPlay defers SET_PARAMETER volume callbacks through its RTP loop; DEBUG logs expose them immediately.
    raop_set_log_level(raop, LOGGER_DEBUG);
    m_raop = raop;

    const QByteArray deviceId = defaultDeviceId();
    if (raop_init2(raop, 0, deviceId.constData(), "") != 0) {
        setError("Failed to initialize RAOP pairing");
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }
    unsigned short port = static_cast<unsigned short>(m_config.basePort > 0 ? m_config.basePort : kDynamicPort);
    if (!m_discovery->start(m_raop, port)) {
        setError(m_discovery->lastError());
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
    const double clamped = std::clamp(volume, 0.0, 1.0);
    m_volume.store(clamped);
#if AIRPLAY_WITH_UXPLAY
    if (m_audioRendererStarted.load()) {
        audio_renderer_set_volume(clamped);
    }
#endif
}

void UxPlayReceiver::setVideoSurface(WId id) {
    Q_UNUSED(id);
}

void UxPlayReceiver::setVideoFrameCallback(FrameCallback callback) {
    m_frameCallback = std::move(callback);
#if AIRPLAY_WITH_UXPLAY
    QMutexLocker locker(&m_rendererMutex);
    if (m_videoFrameBridge) {
        resetVideoFrameBridge();
    }
#endif
}

void UxPlayReceiver::setVideoFitMode(bool enabled) {
    m_videoFitMode.store(enabled);
#if AIRPLAY_WITH_UXPLAY
    m_callbackDispatch.runWithRendererStarted([&] { applyVideoFitModeToRenderer(); });
#endif
}

ReceiverState UxPlayReceiver::state() const {
    return m_state;
}

QString UxPlayReceiver::receiverName() const {
    return m_config.serverName;
}

bool UxPlayReceiver::applyReceiverName(const QString &name) {
    ReceiverNameChangeOperations operations;
    operations.storeName = [&](const QString &requestedName) {
        m_config.serverName = requestedName;
#if AIRPLAY_WITH_UXPLAY
        if (m_discovery) {
            m_discovery->setReceiverName(requestedName);
        }
#endif
        debugLog("applyReceiverName: set config name to \"%s\", state=%d", qPrintable(requestedName), static_cast<int>(m_state));
    };
    operations.restartDiscovery = [&] {
#if AIRPLAY_WITH_UXPLAY
        return m_discovery && m_discovery->restart();
#else
        return true;
#endif
    };
    operations.restartDiscoveryWithRecovery = [&](const QString &recoveryName) {
#if AIRPLAY_WITH_UXPLAY
        return m_discovery && m_discovery->restart(recoveryName);
#else
        Q_UNUSED(recoveryName);
        return true;
#endif
    };
    operations.reportRecoveryFailure = [&](const QString &message) {
        debugLog("applyReceiverName: recovery discovery restart also failed");
        setError(message);
        setState(ReceiverState::Error);
    };

    return applyReceiverNameConfigurationChange(m_state, m_config.serverName, name, operations);
}

bool UxPlayReceiver::applyVideoQuality(const VideoQualitySettings &quality) {
    VideoQualityChangeOperations operations;
    operations.storeQuality = [&](const VideoQualitySettings &requestedQuality) {
        m_config.videoQuality = requestedQuality;
#if AIRPLAY_WITH_UXPLAY
        if (m_discovery) {
            m_discovery->setVideoQuality(requestedQuality);
        }
#endif
    };
    operations.updateActiveAdvertisement = [&](const VideoQualitySettings &requestedQuality) {
#if AIRPLAY_WITH_UXPLAY
        if (m_discovery) {
            m_discovery->setVideoQuality(requestedQuality);
        }
#else
        Q_UNUSED(requestedQuality);
#endif
    };
    operations.restartDiscovery = [&] {
#if AIRPLAY_WITH_UXPLAY
        return m_discovery && m_discovery->restart();
#else
        return true;
#endif
    };
    operations.restartReceiver = [&] {
        stop();
        start();
        return m_state != ReceiverState::Error;
    };

    return applyVideoQualityConfigurationChange(m_state, m_config.videoQuality, quality, operations);
}

#if AIRPLAY_WITH_UXPLAY
void UxPlayReceiver::setStateFromUxPlayCallback(ReceiverState state) {
    setStateFromUxPlayCallback(state, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::setStateFromUxPlayCallback(ReceiverState state, quint64 generation) {
    m_callbackDispatch.runIfCurrent(generation, [&] { setState(state); });
}

quint64 UxPlayReceiver::callbackGenerationForUxPlayCallback() const {
    return m_callbackDispatch.currentGeneration();
}

double UxPlayReceiver::currentVolumeForUxPlayClientVolumeCallback() const {
    return volumeToAirPlayDb(m_volume.load());
}

void UxPlayReceiver::startAudioRendererFromUxPlayCallback(unsigned char *compressionType) {
    startAudioRendererFromUxPlayCallback(compressionType, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::startAudioRendererFromUxPlayCallback(unsigned char *compressionType, quint64 generation) {
    m_callbackDispatch.runWithRendererLock([&] {
        if (!m_callbackDispatch.accepts(generation)) {
            return;
        }
        audio_renderer_start(compressionType);
        m_audioRendererStarted.store(true);
        audio_renderer_set_volume(m_volume.load());
    });
}

void UxPlayReceiver::setVolumeFromUxPlayCallback(double volume) {
    setVolumeFromUxPlayCallback(volume, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::setVolumeFromUxPlayCallback(double volume, quint64 generation) {
    const double clamped = std::clamp(volume, 0.0, 1.0);
    m_callbackDispatch.runWithRendererLock([&] {
        if (!m_callbackDispatch.accepts(generation)) {
            return;
        }
        m_volume.store(clamped);
        if (m_audioRendererStarted.load()) {
            audio_renderer_set_volume(clamped);
        }
        emit volumeChanged(clamped);
    });
}

void UxPlayReceiver::handleLogMessageFromUxPlayCallback(int level, const char *message) {
    handleLogMessageFromUxPlayCallback(level, message, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::handleLogMessageFromUxPlayCallback(int level, const char *message, quint64 generation) {
    if (level != LOGGER_DEBUG || message == nullptr || std::strncmp(message, "volume: ", 8) != 0) {
        return;
    }

    float airPlayDb = 0.0F;
    if (std::sscanf(message + 8, "%f", &airPlayDb) == 1) {
        setVolumeFromUxPlayCallback(volumeFromAirPlayDb(airPlayDb), generation);
    }
}

void UxPlayReceiver::setMetadataFromUxPlayCallback(const void *buffer, int buflen) {
    setMetadataFromUxPlayCallback(buffer, buflen, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::setMetadataFromUxPlayCallback(const void *buffer, int buflen, quint64 generation) {
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

    m_callbackDispatch.runIfCurrent(generation, [&] {
        QPointer<UxPlayReceiver> guardedReceiver(this);
        QMetaObject::invokeMethod(this, [guardedReceiver, generation, title, artist, album] {
            if (guardedReceiver) {
                guardedReceiver->m_callbackDispatch.runIfCurrent(generation, [&] {
                    emit guardedReceiver->trackMetadataChanged(title, artist, album);
                });
            }
        }, Qt::QueuedConnection);
    });
}

void UxPlayReceiver::setCoverArtFromUxPlayCallback(const void *buffer, int buflen) {
    setCoverArtFromUxPlayCallback(buffer, buflen, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::setCoverArtFromUxPlayCallback(const void *buffer, int buflen, quint64 generation) {
    if (!buffer || buflen <= 0) {
        return;
    }

    QByteArray data(static_cast<const char *>(buffer), buflen);
    m_callbackDispatch.runIfCurrent(generation, [&] {
        QPointer<UxPlayReceiver> guardedReceiver(this);
        QMetaObject::invokeMethod(this, [guardedReceiver, generation, data] {
            if (guardedReceiver) {
                guardedReceiver->m_callbackDispatch.runIfCurrent(generation, [&] {
                    emit guardedReceiver->coverArtReceived(data);
                });
            }
        }, Qt::QueuedConnection);
    });
}

void UxPlayReceiver::setProgressFromUxPlayCallback(uint32_t start, uint32_t current, uint32_t end) {
    setProgressFromUxPlayCallback(start, current, end, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::setProgressFromUxPlayCallback(uint32_t start, uint32_t current, uint32_t end, quint64 generation) {
    if (current < start || current > end) {
        return;
    }

    const int positionSec = static_cast<int>((current - start) / 44100);
    const int durationSec = static_cast<int>((end - start) / 44100);
    m_callbackDispatch.runIfCurrent(generation, [&] {
        QPointer<UxPlayReceiver> guardedReceiver(this);
        QMetaObject::invokeMethod(this, [guardedReceiver, generation, positionSec, durationSec] {
            if (guardedReceiver) {
                guardedReceiver->m_callbackDispatch.runIfCurrent(generation, [&] {
                    emit guardedReceiver->progressUpdated(positionSec, durationSec);
                });
            }
        }, Qt::QueuedConnection);
    });
}

void UxPlayReceiver::reportVideoSizeFromUxPlayCallback(int width, int height, quint64 generation) {
    m_callbackDispatch.runIfCurrent(generation, [&] {
        QPointer<UxPlayReceiver> guardedReceiver(this);
        QMetaObject::invokeMethod(this, [guardedReceiver, generation, width, height] {
            if (guardedReceiver) {
                guardedReceiver->m_callbackDispatch.runIfCurrent(generation, [&] {
                    emit guardedReceiver->videoSizeChanged(width, height);
                });
            }
        }, Qt::QueuedConnection);
    });
}

void UxPlayReceiver::stopCoverArtRenderingFromUxPlayCallback() {
    // Cover art is surfaced through Qt, not rendered by the GStreamer video sink.
}

void UxPlayReceiver::handleVideoResetFromUxPlayCallback(int resetType) {
    handleVideoResetFromUxPlayCallback(resetType, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::handleVideoResetFromUxPlayCallback(int resetType, quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [&] {
        if (m_state != ReceiverState::Connected) {
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
    });
}

void UxPlayReceiver::stopVideoPipelineForDisconnect() {
    stopVideoPipelineForDisconnect(m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::stopVideoPipelineForDisconnect(quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [&] {
        if (m_videoRendererStopped.exchange(true)) {
            return;
        }
        video_renderer_stop();
    });
}

void UxPlayReceiver::restartVideoPipelineForConnect() {
    restartVideoPipelineForConnect(m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::restartVideoPipelineForConnect(quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [&] {
        if (m_videoRendererStopped.exchange(false)) {
            video_renderer_start();
        }
    });
}

void UxPlayReceiver::applyVideoFitModeToRenderer() {
    video_renderer_set_force_aspect_ratio(m_videoFitMode.load());
}

void UxPlayReceiver::renderAudioBufferFromCallback(void *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time) {
    renderAudioBufferFromCallback(data, data_len, seqnum, ntp_time, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::renderAudioBufferFromCallback(void *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time,
                                                   quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [&] {
        audio_renderer_render_buffer(static_cast<unsigned char *>(data), data_len, seqnum, ntp_time);
    });
}

void UxPlayReceiver::renderVideoBufferFromCallback(void *data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    renderVideoBufferFromCallback(data, data_len, nal_count, ntp_time, m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::renderVideoBufferFromCallback(void *data, int *data_len, int *nal_count, uint64_t *ntp_time,
                                                   quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [&] {
        video_renderer_render_buffer(static_cast<unsigned char *>(data), data_len, nal_count, ntp_time);
    });
}

void UxPlayReceiver::flushAudioRendererFromCallback() {
    flushAudioRendererFromCallback(m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::flushAudioRendererFromCallback(quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [] { audio_renderer_flush(); });
}

void UxPlayReceiver::flushVideoRendererFromCallback() {
    flushVideoRendererFromCallback(m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::flushVideoRendererFromCallback(quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [] { video_renderer_flush(); });
}

void UxPlayReceiver::pauseVideoRendererFromCallback() {
    pauseVideoRendererFromCallback(m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::pauseVideoRendererFromCallback(quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [] { video_renderer_pause(); });
}

void UxPlayReceiver::resumeVideoRendererFromCallback() {
    resumeVideoRendererFromCallback(m_callbackDispatch.currentGeneration());
}

void UxPlayReceiver::resumeVideoRendererFromCallback(quint64 generation) {
    m_callbackDispatch.runWithRendererStarted(generation, [] { video_renderer_resume(); });
}

int UxPlayReceiver::chooseVideoCodecFromCallback(bool video_is_h265) {
    return chooseVideoCodecFromCallback(video_is_h265, m_callbackDispatch.currentGeneration());
}

int UxPlayReceiver::chooseVideoCodecFromCallback(bool video_is_h265, quint64 generation) {
    int result = -1;
    if (!m_callbackDispatch.runWithRendererStarted(generation, [&] {
        result = video_renderer_choose_codec(false, video_is_h265);
        if (result == 0) {
            attachVideoFrameBridgeToCurrentPipeline();
        }
    })) {
        return -1;
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
void UxPlayReceiver::cleanupUxPlay() {
    m_acceptingCallbacks.store(false);
    m_callbackGeneration.fetch_add(1);
    if (m_callbackContext) {
        m_callbackContext->close();
        m_callbackContext = nullptr;
    }

    if (m_glibTimer) {
        static_cast<QTimer *>(m_glibTimer)->stop();
        delete static_cast<QTimer *>(m_glibTimer);
        m_glibTimer = nullptr;
    }

    if (m_discovery) {
        m_discovery->stop();
    }
    if (m_raop) {
        raop_destroy(static_cast<raop_t *>(m_raop));
        m_raop = nullptr;
    }
    {
        QMutexLocker rendererLocker(&m_rendererMutex);
        resetVideoFrameBridge();
        if (m_renderersStarted.load()) {
            video_renderer_stop();
            video_renderer_destroy();
            audio_renderer_destroy();
            m_renderersStarted.store(false);
            m_audioRendererStarted.store(false);
            m_videoRendererStopped.store(false);
        }
    }
    if (m_logger) {
        logger_destroy(static_cast<logger_t *>(m_logger));
        m_logger = nullptr;
    }
}
#endif

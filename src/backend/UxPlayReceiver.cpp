#include "backend/UxPlayReceiver.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
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
    audio_renderer_render_buffer(data->data, &data->data_len, &data->seqnum, &data->ntp_time_remote);
}

void videoProcess(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    Q_UNUSED(cls);
    Q_UNUSED(ntp);
    video_renderer_render_buffer(data->data, &data->data_len, &data->nal_count, &data->ntp_time_remote);
}

void audioFlush(void *) {
    audio_renderer_flush();
}

void videoFlush(void *) {
    video_renderer_flush();
}

void videoPause(void *) {
    video_renderer_pause();
}

void videoResume(void *) {
    video_renderer_resume();
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

void videoReportSize(void *, float *widthSource, float *heightSource, float *width, float *height) {
    *widthSource = 1920.0F;
    *heightSource = 1080.0F;
    *width = 1920.0F;
    *height = 1080.0F;
}

void connInit(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    const auto generation = receiver->callbackGenerationForUxPlayCallback();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation] {
        if (guardedReceiver) {
            guardedReceiver->setStateFromUxPlayCallback(ReceiverState::Connected, generation);
        }
    }, Qt::QueuedConnection);
}

void connDestroy(void *cls) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    const auto generation = receiver->callbackGenerationForUxPlayCallback();
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, generation] {
        if (guardedReceiver) {
            guardedReceiver->setStateFromUxPlayCallback(ReceiverState::Discoverable, generation);
        }
    }, Qt::QueuedConnection);
}

void connReset(void *, int reason) {
    switch (reason) {
    case 1:
        printf("*** ERROR lost connection with client (network problem?)\n");
        break;
    case 2:
        printf("*** ERROR Unsupported HLS streaming source\n");
        break;
    default:
        break;
    }
}

void connFeedback(void *) {
    static int missed_feedback = 0;
    missed_feedback = 0;
}

void videoReset(void *cls, reset_type_t type) {
    auto *receiver = static_cast<UxPlayReceiver *>(cls);
    receiver->handleVideoResetFromUxPlayCallback(static_cast<int>(type));
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

    if (!buffer || buflen <= 0) {
        return;
    }

    video_renderer_choose_codec(true, false);
    int buflen_mut = buflen;
    video_renderer_display_jpeg(buffer, &buflen_mut);

    QByteArray data(static_cast<const char *>(buffer), buflen_mut);
    QPointer<UxPlayReceiver> guardedReceiver(receiver);
    QMetaObject::invokeMethod(receiver, [guardedReceiver, data] {
        if (guardedReceiver) {
            emit guardedReceiver->coverArtReceived(data);
        }
    }, Qt::QueuedConnection);
}

void audioStopCoverartRendering(void *cls) {
    videoReset(cls, RESET_TYPE_RTP_SHUTDOWN);
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

int videoSetCodec(void *, video_codec_t codec) {
    bool video_is_h265 = (codec == VIDEO_CODEC_H265);
    return video_renderer_choose_codec(false, video_is_h265);
}
} // namespace
#endif

UxPlayReceiver::UxPlayReceiver(UxPlayReceiverConfig config, QObject *parent)
    : AirPlayReceiver(parent), m_config(std::move(config)) {}

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
    videoflip_t videoFlip[2] = {NONE, NONE};
    video_renderer_init(logger, serverName.constData(), videoFlip, "h264parse", "",
                        "decodebin", "videoconvert", videoSink.constData(), "", false, kVideoSync, false, false,
                        kPlaybinVersion, nullptr);
    video_renderer_start();
    audio_renderer_init(logger, audioSink.constData(), &kAudioSync, &kVideoSync, "");
    m_renderersStarted = true;

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
    if (raop_start_httpd(raop, &port) < 0) {
        setError("Failed to start RAOP HTTP server");
        cleanupUxPlay();
        setState(ReceiverState::Error);
        return;
    }
    m_raopHttpdStarted = true;
    raop_set_port(raop, port);
    m_raopPort = port;

    if (!startDiscoveryBroadcast(m_raopPort)) {
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
        audio_renderer_set_volume(m_volume.load());
    }
#endif
}

void UxPlayReceiver::setVideoSurface(WId id) {
    m_videoSurfaceId = id;
#if AIRPLAY_WITH_UXPLAY
    video_renderer_set_window_handle(reinterpret_cast<void *>(static_cast<uintptr_t>(id)));
#endif
}

ReceiverState UxPlayReceiver::state() const {
    return m_state;
}

QString UxPlayReceiver::receiverName() const {
    return m_config.serverName;
}

bool UxPlayReceiver::applyReceiverName(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    if (m_config.serverName == trimmed) {
        return true;
    }

    const QString previousName = m_config.serverName;
    m_config.serverName = trimmed;

    if (m_state == ReceiverState::Idle) {
        return true;
    }

    if (m_state == ReceiverState::Connected) {
        stop();
        start();
        if (m_state == ReceiverState::Error) {
            m_config.serverName = previousName;
            return false;
        }
        return true;
    }

#if AIRPLAY_WITH_UXPLAY
    if (!restartDiscoveryBroadcast()) {
        m_config.serverName = previousName;
        restartDiscoveryBroadcast();
        return false;
    }
    return true;
#else
    return true;
#endif
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
    if (!m_acceptingCallbacks.load()) {
        return;
    }
    audio_renderer_start(compressionType);
    m_audioRendererStarted.store(true);
    audio_renderer_set_volume(m_volume.load());
}

void UxPlayReceiver::setVolumeFromUxPlayCallback(double volume) {
    if (!m_acceptingCallbacks.load()) {
        return;
    }
    m_volume.store(volume);
    if (m_audioRendererStarted.load()) {
        audio_renderer_set_volume(m_volume.load());
    }
}

void UxPlayReceiver::handleVideoResetFromUxPlayCallback(int resetType) {
    const auto restartVideoRenderer = [this] {
        video_renderer_stop();
        video_renderer_destroy();

        auto *logger = static_cast<logger_t *>(m_logger);
        const QByteArray serverName = m_config.serverName.toUtf8();
        const QByteArray videoSink = m_config.videoSink.toUtf8();
        videoflip_t videoFlip[2] = {NONE, NONE};
        video_renderer_init(logger, serverName.constData(), videoFlip, "h264parse", "",
                            "decodebin", "videoconvert", videoSink.constData(), "", false, kVideoSync, false, false,
                            kPlaybinVersion, nullptr);
        video_renderer_start();
        video_renderer_choose_codec(false, false);
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
bool UxPlayReceiver::startDiscoveryBroadcast(unsigned short port) {
    if (!m_raop) {
        setError("Cannot register DNS-SD services before RAOP initialization");
        return false;
    }
    if (m_dnssd) {
        return true;
    }

    int dnssdError = 0;
    const QByteArray serverName = m_config.serverName.toUtf8();
    const QByteArray hwAddress = defaultHardwareAddress();
    auto *dnssd = dnssd_init(serverName.constData(), serverName.size(), hwAddress.constData(), hwAddress.size(), &dnssdError, 0);
    if (dnssdError || !dnssd) {
        setError(QString("Failed to initialize DNS-SD: %1").arg(dnssdError));
        return false;
    }

    m_dnssd = dnssd;
    raop_set_dnssd(static_cast<raop_t *>(m_raop), dnssd);

    int registerError = dnssd_register_raop(dnssd, port);
    if (registerError == 0) {
        registerError = dnssd_register_airplay(dnssd, port);
    }
    if (registerError != 0) {
        setError(QString("Failed to register DNS-SD services: %1").arg(registerError));
        return false;
    }

    return true;
}

void UxPlayReceiver::stopDiscoveryBroadcast() {
    if (!m_dnssd) {
        return;
    }

    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    dnssd_destroy(dnssd);
    m_dnssd = nullptr;
}

bool UxPlayReceiver::restartDiscoveryBroadcast() {
    if (!m_raop || !m_raopPort) {
        return false;
    }
    if (!m_dnssd) {
        return startDiscoveryBroadcast(m_raopPort);
    }

    auto *oldDnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_unregister_raop(oldDnssd);
    dnssd_unregister_airplay(oldDnssd);

    int dnssdError = 0;
    const QByteArray serverName = m_config.serverName.toUtf8();
    const QByteArray hwAddress = defaultHardwareAddress();
    auto *newDnssd = dnssd_init(serverName.constData(), serverName.size(), hwAddress.constData(), hwAddress.size(), &dnssdError, 0);
    if (dnssdError || !newDnssd) {
        dnssd_register_raop(oldDnssd, m_raopPort);
        dnssd_register_airplay(oldDnssd, m_raopPort);
        setError(QString("Failed to initialize DNS-SD: %1").arg(dnssdError));
        return false;
    }

    raop_set_dnssd(static_cast<raop_t *>(m_raop), newDnssd);
    int registerError = dnssd_register_raop(newDnssd, m_raopPort);
    if (registerError == 0) {
        registerError = dnssd_register_airplay(newDnssd, m_raopPort);
    }
    if (registerError != 0) {
        dnssd_unregister_raop(newDnssd);
        dnssd_unregister_airplay(newDnssd);
        dnssd_destroy(newDnssd);
        raop_set_dnssd(static_cast<raop_t *>(m_raop), oldDnssd);
        dnssd_register_raop(oldDnssd, m_raopPort);
        dnssd_register_airplay(oldDnssd, m_raopPort);
        setError(QString("Failed to register DNS-SD services: %1").arg(registerError));
        return false;
    }

    dnssd_destroy(oldDnssd);
    m_dnssd = newDnssd;
    return true;
}

void UxPlayReceiver::cleanupUxPlay() {
    m_acceptingCallbacks.store(false);
    m_callbackGeneration.fetch_add(1);

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
    if (m_renderersStarted) {
        video_renderer_stop();
        video_renderer_destroy();
        audio_renderer_destroy();
        m_renderersStarted = false;
        m_audioRendererStarted.store(false);
    }
    if (m_logger) {
        logger_destroy(static_cast<logger_t *>(m_logger));
        m_logger = nullptr;
    }
}
#endif

#pragma once

#include <atomic>

#include <QString>
#include <QMutex>
#include <QRecursiveMutex>

#include "backend/AirPlayReceiver.h"

class MdnsPublisher;

struct UxPlayReceiverConfig {
    QString serverName = "AirPlay Receiver";
    QString videoSink = "autovideosink";
    QString audioSink = "autoaudiosink";
    int basePort = 0;
};

class UxPlayReceiver : public AirPlayReceiver {
    Q_OBJECT

public:
    explicit UxPlayReceiver(UxPlayReceiverConfig config = {}, QObject *parent = nullptr);
    ~UxPlayReceiver() override;

    void start() override;
    void stop() override;
    void setVolume(double volume) override;
    void setVideoSurface(WId id) override;
    void setVideoFitMode(bool enabled) override;
    ReceiverState state() const override;
    QString receiverName() const override;
    bool applyReceiverName(const QString &name) override;
#if AIRPLAY_WITH_UXPLAY
    void setStateFromUxPlayCallback(ReceiverState state);
    void setStateFromUxPlayCallback(ReceiverState state, quint64 generation);
    quint64 callbackGenerationForUxPlayCallback() const;
    void startAudioRendererFromUxPlayCallback(unsigned char *compressionType);
    void setVolumeFromUxPlayCallback(double volume);
    void setCoverArtFromUxPlayCallback(const void *buffer, int buflen);
    void stopCoverArtRenderingFromUxPlayCallback();
    void handleVideoResetFromUxPlayCallback(int resetType);
    void handleVideoResetFromUxPlayCallback(int resetType, quint64 generation);
    void stopVideoPipelineForDisconnect();
    void restartVideoPipelineForConnect();
    void renderAudioBufferFromCallback(void *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time);
    void renderVideoBufferFromCallback(void *data, int *data_len, int *nal_count, uint64_t *ntp_time);
    void flushAudioRendererFromCallback();
    void flushVideoRendererFromCallback();
    void pauseVideoRendererFromCallback();
    void resumeVideoRendererFromCallback();
    int chooseVideoCodecFromCallback(bool video_is_h265);
#endif

signals:
    void trackMetadataChanged(const QString &title, const QString &artist, const QString &album);
    void coverArtReceived(const QByteArray &data);
    void progressUpdated(int positionSec, int durationSec);

private:
    void setState(ReceiverState state);
    void setError(QString error);
#if AIRPLAY_WITH_UXPLAY
    void cleanupUxPlay();
    void bindVideoSurfaceToRenderer();
    void applyVideoFitModeToRenderer();
    bool createDiscoveryBroadcast();
    bool registerDiscoveryBroadcast(unsigned short port);
    void stopDiscoveryBroadcast();
    void unregisterDiscoveryBroadcast();
    void destroyDiscoveryBroadcast();
    bool restartDiscoveryBroadcast();
    void cancelPendingDiscoveryRestart();
#endif

    UxPlayReceiverConfig m_config;
    ReceiverState m_state = ReceiverState::Idle;
    QString m_error;
    WId m_videoSurfaceId = 0;
    std::atomic<double> m_volume = 1.0;
    std::atomic_bool m_videoFitMode = false;
#if AIRPLAY_WITH_UXPLAY
    void *m_raop = nullptr;
    void *m_dnssd = nullptr;
    void *m_logger = nullptr;
    bool m_raopHttpdInitialized = false;
    bool m_raopHttpdStarted = false;
    unsigned short m_raopPort = 0;
    std::atomic_bool m_renderersStarted = false;
    std::atomic_bool m_videoRendererStopped = false;
    std::atomic_bool m_audioRendererStarted = false;
    std::atomic_bool m_acceptingCallbacks = false;
    std::atomic<quint64> m_callbackGeneration = 0;
    std::atomic_bool m_discoveryRestartPending = false;
    QObject *m_glibTimer = nullptr;
    QTimer *m_discoveryRestartTimer = nullptr;
    QRecursiveMutex m_rendererMutex;
    QString m_pendingDiscoveryRecoveryName; // last stable advertised name, used for async rollback if rename restart fails
    MdnsPublisher *m_mdnsPublisher = nullptr;
#endif
};

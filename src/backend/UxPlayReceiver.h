#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <QString>
#include <QMutex>
#include <QRecursiveMutex>
#include <QWaitCondition>

#include "backend/AirPlayReceiver.h"
#include "backend/UxPlayCallbackDispatch.h"

class MdnsPublishing;
class UxPlayDiscovery;
class VideoFrameBridge;

struct UxPlayReceiverConfig {
    QString serverName = "AirPlay Receiver";
    QString videoSink = "autovideosink";
    QString audioSink = "autoaudiosink";
    int basePort = 0;
    VideoQualitySettings videoQuality;
    MdnsPublishing *mdnsPublisher = nullptr;
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
    void setVideoFrameCallback(FrameCallback callback) override;
    void setVideoFitMode(bool enabled) override;
    ReceiverState state() const override;
    QString receiverName() const override;
    bool applyReceiverName(const QString &name) override;
    bool applyVideoQuality(const VideoQualitySettings &quality) override;
#if AIRPLAY_WITH_UXPLAY
    struct CallbackContext {
        CallbackContext(UxPlayReceiver *receiver, quint64 generation);

        bool enter(UxPlayReceiver **receiverOut);
        void leave();
        void close();

        quint64 generation = 0;

    private:
        UxPlayReceiver *m_receiver = nullptr;
        bool m_closed = false;
        int m_inFlight = 0;
        QMutex m_mutex;
        QWaitCondition m_idle;
    };

    void setStateFromUxPlayCallback(ReceiverState state);
    void setStateFromUxPlayCallback(ReceiverState state, quint64 generation);
    quint64 callbackGenerationForUxPlayCallback() const;
    double currentVolumeForUxPlayClientVolumeCallback() const;
    void startAudioRendererFromUxPlayCallback(unsigned char *compressionType);
    void startAudioRendererFromUxPlayCallback(unsigned char *compressionType, quint64 generation);
    void setVolumeFromUxPlayCallback(double volume);
    void setVolumeFromUxPlayCallback(double volume, quint64 generation);
    void handleLogMessageFromUxPlayCallback(int level, const char *message);
    void handleLogMessageFromUxPlayCallback(int level, const char *message, quint64 generation);
    void setMetadataFromUxPlayCallback(const void *buffer, int buflen);
    void setMetadataFromUxPlayCallback(const void *buffer, int buflen, quint64 generation);
    void setCoverArtFromUxPlayCallback(const void *buffer, int buflen);
    void setCoverArtFromUxPlayCallback(const void *buffer, int buflen, quint64 generation);
    void setProgressFromUxPlayCallback(uint32_t start, uint32_t current, uint32_t end);
    void setProgressFromUxPlayCallback(uint32_t start, uint32_t current, uint32_t end, quint64 generation);
    void stopCoverArtRenderingFromUxPlayCallback();
    void reportVideoSizeFromUxPlayCallback(int width, int height, quint64 generation);
    void handleVideoResetFromUxPlayCallback(int resetType);
    void handleVideoResetFromUxPlayCallback(int resetType, quint64 generation);
    void stopVideoPipelineForDisconnect();
    void stopVideoPipelineForDisconnect(quint64 generation);
    void restartVideoPipelineForConnect();
    void restartVideoPipelineForConnect(quint64 generation);
    void renderAudioBufferFromCallback(void *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time);
    void renderAudioBufferFromCallback(void *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time, quint64 generation);
    void renderVideoBufferFromCallback(void *data, int *data_len, int *nal_count, uint64_t *ntp_time);
    void renderVideoBufferFromCallback(void *data, int *data_len, int *nal_count, uint64_t *ntp_time, quint64 generation);
    void flushAudioRendererFromCallback();
    void flushAudioRendererFromCallback(quint64 generation);
    void flushVideoRendererFromCallback();
    void flushVideoRendererFromCallback(quint64 generation);
    void pauseVideoRendererFromCallback();
    void pauseVideoRendererFromCallback(quint64 generation);
    void resumeVideoRendererFromCallback();
    void resumeVideoRendererFromCallback(quint64 generation);
    int chooseVideoCodecFromCallback(bool video_is_h265);
    int chooseVideoCodecFromCallback(bool video_is_h265, quint64 generation);
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
    void applyVideoFitModeToRenderer();
    void resetVideoFrameBridge();
    void attachVideoFrameBridgeToCurrentPipeline();
#endif

    UxPlayReceiverConfig m_config;
    ReceiverState m_state = ReceiverState::Idle;
    QString m_error;
    std::atomic<double> m_volume = 1.0;
    std::atomic_bool m_videoFitMode = false;
    AirPlayReceiver::FrameCallback m_frameCallback;
    VideoFrameBridge *m_videoFrameBridge = nullptr;
#if AIRPLAY_WITH_UXPLAY
    void *m_raop = nullptr;
    void *m_logger = nullptr;
    std::atomic_bool m_renderersStarted = false;
    std::atomic_bool m_videoRendererStopped = false;
    std::atomic_bool m_audioRendererStarted = false;
    std::atomic_bool m_acceptingCallbacks = false;
    std::atomic<quint64> m_callbackGeneration = 0;
    QObject *m_glibTimer = nullptr;
    QRecursiveMutex m_rendererMutex;
    UxPlayCallbackDispatch m_callbackDispatch;
    CallbackContext *m_callbackContext = nullptr;
    std::vector<std::unique_ptr<CallbackContext>> m_callbackContexts;
    UxPlayDiscovery *m_discovery = nullptr;
#endif
};

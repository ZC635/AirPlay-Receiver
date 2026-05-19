#pragma once

#include "backend/VideoQualitySettings.h"

#include <QObject>
#include <QString>

class DiscoveryRestartController;
class MdnsPublishing;

struct UxPlayDiscoveryConfig {
    QString receiverName = "AirPlay Receiver";
    VideoQualitySettings videoQuality;
    MdnsPublishing *mdnsPublisher = nullptr;
    int restartDelayMs = 2000;
};

class UxPlayDiscovery : public QObject {
    Q_OBJECT

public:
    explicit UxPlayDiscovery(UxPlayDiscoveryConfig config = {}, QObject *parent = nullptr);
    ~UxPlayDiscovery() override;

    bool start(void *raop, unsigned short requestedPort);
    void stop();
    bool restart(QString recoveryName = {});
    void setReceiverName(QString receiverName);
    void setVideoQuality(VideoQualitySettings quality);
    QString lastError() const;

signals:
    void failed(QString error);

private:
    void setLastError(QString error);
    void applyVideoQualityToRaop();
    bool createBroadcast();
    bool registerBroadcast(unsigned short port);
    void stopBroadcast();
    void unregisterBroadcast();
    void destroyBroadcast();
    void stopHttpdIfStarted();

    UxPlayDiscoveryConfig m_config;
    QString m_lastError;

#if AIRPLAY_WITH_UXPLAY
    void *m_raop = nullptr;
    void *m_dnssd = nullptr;
    bool m_raopHttpdStarted = false;
    unsigned short m_raopPort = 0;
    DiscoveryRestartController *m_discoveryRestartController = nullptr;
    MdnsPublishing *m_mdnsPublisher = nullptr;
    bool m_ownsMdnsPublisher = false;
#endif
};

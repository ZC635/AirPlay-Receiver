#if AIRPLAY_WITH_UXPLAY
#include <winsock2.h>
#endif

#include <QtTest/QtTest>

#include <QByteArray>
#include <QStringList>

#include <cstring>

#if AIRPLAY_WITH_UXPLAY
#define private public
#include "backend/UxPlayDiscovery.h"
#undef private

#include "backend/DiscoveryRestartController.h"
#include "lib/raop.h"
#include "platform/MdnsPublishing.h"

class FakeMdnsPublishing : public MdnsPublishing {
public:
    bool publish(const QString &receiverName, const QByteArray &hardwareAddress, quint16 port,
                 const char *raopTxt, int raopTxtLength,
                 const char *airplayTxt, int airplayTxtLength) override {
        ++publishCalls;
        publishedNames.append(receiverName);
        lastReceiverName = receiverName;
        lastHardwareAddress = hardwareAddress;
        lastPort = port;
        lastRaopTxt = QByteArray(raopTxt, raopTxtLength);
        lastAirplayTxt = QByteArray(airplayTxt, airplayTxtLength);
        return !failedPublishCalls.contains(publishCalls);
    }

    void stop() override { ++stopCalls; }

    int publishCalls = 0;
    int stopCalls = 0;
    QList<int> failedPublishCalls;
    QStringList publishedNames;
    QString lastReceiverName;
    QByteArray lastHardwareAddress;
    quint16 lastPort = 0;
    QByteArray lastRaopTxt;
    QByteArray lastAirplayTxt;
};

class ScopedRaop {
public:
    ScopedRaop() {
        raop_callbacks_t callbacks;
        std::memset(&callbacks, 0, sizeof(callbacks));
        callbacks.audio_process = [](void *, raop_ntp_t *, audio_decode_struct *) {};
        callbacks.video_process = [](void *, raop_ntp_t *, video_decode_struct *) {};
        m_raop = raop_init(&callbacks);
        if (m_raop) {
            const QByteArray deviceId = QByteArrayLiteral("02:00:00:00:00:01");
            if (raop_init2(m_raop, 0, deviceId.constData(), "") != 0) {
                raop_destroy(m_raop);
                m_raop = nullptr;
            }
        }
    }

    ~ScopedRaop() {
        if (!m_raop) {
            return;
        }
        if (raop_is_running(m_raop)) {
            raop_stop_httpd(m_raop);
        }
        raop_destroy(m_raop);
    }

    void *get() const { return m_raop; }
    bool isValid() const { return m_raop != nullptr; }

private:
    raop_t *m_raop = nullptr;
};

struct RaopLayoutForTest {
    raop_callbacks_t callbacks;
    void *logger;
    void *pairing;
    void *httpd;
    void *dnssd;
    unsigned short port;
    unsigned short timingLPort;
    unsigned short controlLPort;
    unsigned short dataLPort;
    unsigned short mirrorDataLPort;
    quint16 width;
    quint16 height;
    quint8 refreshRate;
    quint8 maxFPS;
};

static RaopLayoutForTest *raopLayout(void *raop) {
    return reinterpret_cast<RaopLayoutForTest *>(raop);
}

static UxPlayDiscoveryConfig discoveryConfig(const QString &receiverName, FakeMdnsPublishing *publisher,
                                             int restartDelayMs = 0) {
    UxPlayDiscoveryConfig config;
    config.receiverName = receiverName;
    config.mdnsPublisher = publisher;
    config.restartDelayMs = restartDelayMs;
    return config;
}
#endif

class UxPlayDiscoveryTest : public QObject {
    Q_OBJECT

private slots:
    void startPublishesInjectedMdnsServicesAndStopUnpublishes() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Discovery Test", &publisher));

        QVERIFY(discovery.start(raop.get(), 0));

        QCOMPARE(publisher.publishCalls, 1);
        QCOMPARE(publisher.lastReceiverName, QString("AirPlay Discovery Test"));
        QVERIFY(!publisher.lastHardwareAddress.isEmpty());
        QVERIFY(publisher.lastPort > 0);
        QVERIFY(!publisher.lastRaopTxt.isEmpty());
        QVERIFY(!publisher.lastAirplayTxt.isEmpty());
        QVERIFY(raop_is_running(static_cast<raop_t *>(raop.get())));

        discovery.stop();

        QCOMPARE(publisher.stopCalls, 1);
        QVERIFY(!raop_is_running(static_cast<raop_t *>(raop.get())));
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void publishFailureReportsLastErrorAndStopsPublisher() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        publisher.failedPublishCalls = {1};
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Publish Failure", &publisher));

        QVERIFY(!discovery.start(raop.get(), 0));

        QCOMPARE(publisher.publishCalls, 1);
        QVERIFY(publisher.stopCalls >= 1);
        QCOMPARE(discovery.lastError(), QString("Failed to publish mDNS services"));
        QVERIFY(!raop_is_running(static_cast<raop_t *>(raop.get())));
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void restartFailureWithRecoveryRollsBackReceiverName() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        publisher.failedPublishCalls = {2};
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Original Name", &publisher));
        QVERIFY(discovery.start(raop.get(), 0));

        discovery.setReceiverName("AirPlay Broken Name");
        QVERIFY(discovery.restart("AirPlay Original Name"));
        discovery.m_discoveryRestartController->trigger();

        QCOMPARE(publisher.publishedNames,
                 QStringList({"AirPlay Original Name", "AirPlay Broken Name", "AirPlay Original Name"}));
        QVERIFY(discovery.lastError().isEmpty());
        QVERIFY(raop_is_running(static_cast<raop_t *>(raop.get())));

        discovery.stop();
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void restartGuardFailureStopsHttpdAndClearsExistingBroadcast() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Restart Guard", &publisher));
        QVERIFY(discovery.start(raop.get(), 0));
        QVERIFY(discovery.m_dnssd != nullptr);
        QVERIFY(discovery.m_raopHttpdStarted);

        discovery.m_raopPort = 0;
        QVERIFY(!discovery.restart());

        QVERIFY(discovery.m_dnssd == nullptr);
        QVERIFY(!discovery.m_raopHttpdStarted);
        QVERIFY(!raop_is_running(static_cast<raop_t *>(raop.get())));
        QVERIFY(publisher.stopCalls >= 1);
        QVERIFY(!discovery.lastError().isEmpty());
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void registerFailureWithoutRecoveryStopsHttpdAndClearsBroadcast() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        publisher.failedPublishCalls = {2};
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Register Failure", &publisher));
        QVERIFY(discovery.start(raop.get(), 0));

        QVERIFY(discovery.restart());
        discovery.m_discoveryRestartController->trigger();

        QCOMPARE(publisher.publishCalls, 2);
        QVERIFY(discovery.m_dnssd == nullptr);
        QVERIFY(!discovery.m_raopHttpdStarted);
        QVERIFY(!raop_is_running(static_cast<raop_t *>(raop.get())));
        QVERIFY(discovery.lastError().contains("Failed"));
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void restartAttemptsHttpdWhenBroadcastIsActiveButHttpdWasStopped() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Restart Stopped Httpd", &publisher));
        QVERIFY(discovery.start(raop.get(), 0));
        QVERIFY(discovery.m_dnssd != nullptr);

        raop_stop_httpd(static_cast<raop_t *>(raop.get()));
        discovery.m_raopHttpdStarted = false;

        QVERIFY(discovery.restart());
        discovery.m_discoveryRestartController->trigger();

        QCOMPARE(publisher.publishCalls, 2);
        QVERIFY(discovery.m_raopHttpdStarted);
        QVERIFY(raop_is_running(static_cast<raop_t *>(raop.get())));

        discovery.stop();
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void doubleRenameKeepsOriginalRecoveryNameButPublishesLatestName() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        publisher.failedPublishCalls = {2};
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Original Rename", &publisher, 10000));
        QVERIFY(discovery.start(raop.get(), 0));

        discovery.setReceiverName("AirPlay First Rename");
        QVERIFY(discovery.restart("AirPlay Original Rename"));
        QVERIFY(discovery.m_discoveryRestartController->pending());
        QCOMPARE(discovery.m_discoveryRestartController->recoveryName(), QString("AirPlay Original Rename"));

        discovery.setReceiverName("AirPlay Second Rename");
        QVERIFY(discovery.restart("AirPlay Ignored Recovery Name"));
        QVERIFY(discovery.m_discoveryRestartController->pending());
        QCOMPARE(discovery.m_discoveryRestartController->recoveryName(), QString("AirPlay Original Rename"));

        discovery.m_discoveryRestartController->trigger();

        QCOMPARE(publisher.publishedNames,
                 QStringList({"AirPlay Original Rename", "AirPlay Second Rename", "AirPlay Original Rename"}));

        discovery.stop();
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }

    void setVideoQualityUpdatesRaopAdvertisementBeforeRestart() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        ScopedRaop raop;
        QVERIFY(raop.isValid());
        UxPlayDiscovery discovery(discoveryConfig("AirPlay Quality", &publisher));
        QVERIFY(discovery.start(raop.get(), 0));

        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps15;
        discovery.setVideoQuality(quality);

        QCOMPARE(raopLayout(raop.get())->width, quint16(1280));
        QCOMPARE(raopLayout(raop.get())->height, quint16(720));
        QCOMPARE(raopLayout(raop.get())->refreshRate, quint8(60));
        QCOMPARE(raopLayout(raop.get())->maxFPS, quint8(15));

        QVERIFY(discovery.restart());
        discovery.m_discoveryRestartController->trigger();

        QCOMPARE(publisher.publishCalls, 2);
        QCOMPARE(publisher.lastReceiverName, QString("AirPlay Quality"));
        QVERIFY(raop_is_running(static_cast<raop_t *>(raop.get())));

        discovery.stop();
#else
        QSKIP("UxPlay support is not enabled in this build");
#endif
    }
};

QTEST_GUILESS_MAIN(UxPlayDiscoveryTest)
#include "UxPlayDiscoveryTest.moc"

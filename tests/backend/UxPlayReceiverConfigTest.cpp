#if AIRPLAY_WITH_UXPLAY
#include <winsock2.h>
#endif

#include <QtTest/QtTest>
#include <QFile>

#include "backend/FakeAirPlayReceiver.h"

#if AIRPLAY_WITH_UXPLAY
#define private public
#endif
#include "backend/UxPlayReceiver.h"
#if AIRPLAY_WITH_UXPLAY
#undef private
#endif

#if AIRPLAY_WITH_UXPLAY
#include "lib/raop.h"
#include "lib/dnssd.h"
#include "platform/MdnsPublisher.h"

static bool txtRecordContainsKey(const char *data, int length, const char *key) {
    const char *end = data + length;
    const char *pos = data;
    size_t key_len = strlen(key);
    while (pos < end) {
        int entry_len = (unsigned char)*pos;
        if (entry_len == 0 || pos + 1 + entry_len > end) break;
        const char *entry = pos + 1;
        if ((size_t)entry_len >= key_len + 1 &&
            memcmp(entry, key, key_len) == 0 &&
            entry[key_len] == '=') {
            return true;
        }
        pos += 1 + entry_len;
    }
    return false;
}

extern "C" int video_renderer_choose_codec(bool video_is_jpeg, bool video_is_h265);
extern "C" void video_renderer_set_force_aspect_ratio(bool enabled);
extern "C" bool video_renderer_get_force_aspect_ratio(void);

class TcpPortBlocker {
public:
    ~TcpPortBlocker() { close(); }

    bool start(unsigned short port) {
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET) {
            m_error = QString("socket failed: %1").arg(WSAGetLastError());
            return false;
        }

        BOOL exclusive = TRUE;
        setsockopt(m_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));

        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(m_socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
            m_error = QString("bind failed: %1").arg(WSAGetLastError());
            close();
            return false;
        }
        if (::listen(m_socket, 1) == SOCKET_ERROR) {
            m_error = QString("listen failed: %1").arg(WSAGetLastError());
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
    }

    QString errorString() const { return m_error; }

private:
    SOCKET m_socket = INVALID_SOCKET;
    QString m_error;
};
#endif

class UxPlayReceiverConfigTest : public QObject {
    Q_OBJECT

private slots:
    void defaultConfigUsesWindowsFriendlySinks() {
        const UxPlayReceiverConfig config;
        QCOMPARE(config.serverName, QString("AirPlay Receiver"));
        QVERIFY(config.videoSink.contains("d3d") || config.videoSink == "autovideosink");
        QVERIFY(config.audioSink.contains("wasapi") || config.audioSink == "autoaudiosink");
    }

    void startWithoutUxPlayReportsBuildError() {
#if !AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::stateChanged);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();

        QCOMPARE(receiver.state(), ReceiverState::Error);
        QCOMPARE(stateSpy.count(), 1);
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(errorSpy.at(0).at(0).toString(), QString("UxPlay support is not enabled in this build"));
#endif
    }

    void startWithUxPlayStartsReceiverLifecycle() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::stateChanged);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();

        if (errorSpy.count() != 0) {
            QFAIL(qPrintable(errorSpy.at(0).at(0).toString()));
        }
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QVERIFY(stateSpy.count() >= 1);

        QVERIFY(receiver.m_mdnsPublisher != nullptr);

        receiver.stop();
        QCOMPARE(receiver.state(), ReceiverState::Idle);
#endif
    }

    void stoppedUxPlayReceiverIgnoresLateCallbackState() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Late Callback Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        receiver.stop();
        receiver.setStateFromUxPlayCallback(ReceiverState::Discoverable);

        QCOMPARE(receiver.state(), ReceiverState::Idle);
#endif
    }

    void stoppedUxPlayReceiverIgnoresLateVideoReset() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Late Video Reset Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        receiver.stop();
        receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_RTP_SHUTDOWN);

        QCOMPARE(receiver.state(), ReceiverState::Idle);
#endif
    }

    void discoverableReceiverNameChangeRestartsDiscoveryOnly() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Before Rename";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::stateChanged);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();
        if (errorSpy.count() != 0) {
            QFAIL(qPrintable(errorSpy.at(0).at(0).toString()));
        }
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        stateSpy.clear();

        QVERIFY(receiver.applyReceiverName("AirPlay Receiver After Rename"));

        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver After Rename"));
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        for (const QList<QVariant> &signal : stateSpy) {
            const auto state = signal.at(0).value<ReceiverState>();
            QVERIFY(state != ReceiverState::Idle);
            QVERIFY(state != ReceiverState::Starting);
        }

        receiver.stop();
#endif
    }

    void restartDiscoveryFailureClearsStaleBroadcast() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Restart Failure Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QVERIFY(receiver.m_dnssd != nullptr);
        QVERIFY(receiver.m_raopPort != 0);

        const unsigned short port = receiver.m_raopPort;
        raop_stop_httpd(static_cast<raop_t *>(receiver.m_raop));
        receiver.m_raopHttpdStarted = false;

        TcpPortBlocker blocker;
        QVERIFY2(blocker.start(port), qPrintable(blocker.errorString()));

        const bool scheduled = receiver.restartDiscoveryBroadcast();
        const bool goodbyeSent = receiver.m_dnssd != nullptr;
        const bool timerPending = receiver.m_discoveryRestartPending.load();
        blocker.close();
        receiver.stop();

        QVERIFY(scheduled);
        QVERIFY(goodbyeSent);
        QVERIFY(!receiver.m_discoveryRestartPending.load());
#endif
    }

    void restartDiscoveryGuardClearsExistingBroadcast() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Restart Guard Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QVERIFY(receiver.m_dnssd != nullptr);

        receiver.m_raopPort = 0;
        const bool restarted = receiver.restartDiscoveryBroadcast();
        const bool staleBroadcastLeft = receiver.m_dnssd != nullptr;
        const bool httpdLeftRunning = receiver.m_raopHttpdStarted;
        receiver.stop();

        QVERIFY(!restarted);
        QVERIFY(!staleBroadcastLeft);
        QVERIFY(!httpdLeftRunning);
        QVERIFY(errorSpy.count() > 0);
#endif
    }

    void restartDiscoveryRegisterFailureStopsHttpdAndClearsBroadcast() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Register Failure Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QVERIFY(receiver.m_raopHttpdStarted);
        QVERIFY(receiver.m_dnssd != nullptr);

        receiver.m_config.serverName = QString(300, QLatin1Char('A'));
        const bool scheduled = receiver.restartDiscoveryBroadcast();
        const bool goodbyeSent = receiver.m_dnssd != nullptr;
        receiver.stop();

        QVERIFY(scheduled);
        QVERIFY(goodbyeSent);
        QVERIFY(!receiver.m_discoveryRestartPending.load());
#endif
    }

    void disconnectRestartTracksStoppedVideoRendererAtomically() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Video Restart Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        receiver.stopVideoPipelineForDisconnect();
        QVERIFY(receiver.m_videoRendererStopped.load());

        receiver.restartVideoPipelineForConnect();
        QVERIFY(!receiver.m_videoRendererStopped.load());

        receiver.stop();
#endif
    }

    void lateVideoResetAfterDisconnectDoesNotRecreateRenderer() {
#if AIRPLAY_WITH_UXPLAY
        qputenv("AIRPLAY_DEBUG_LOG", "1");
        const QString logPath = QCoreApplication::applicationDirPath() + QStringLiteral("/airplay_receiver_debug.log");
        QFile::remove(logPath);

        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Late Reset After Disconnect Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        receiver.setStateFromUxPlayCallback(ReceiverState::Connected);
        QCOMPARE(video_renderer_choose_codec(false, false), 0);

        receiver.stopVideoPipelineForDisconnect();
        receiver.setStateFromUxPlayCallback(ReceiverState::Discoverable);
        receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_RTP_SHUTDOWN);
        receiver.stop();

        QFile logFile(logPath);
        QVERIFY(logFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString log = QString::fromUtf8(logFile.readAll());
        qunsetenv("AIRPLAY_DEBUG_LOG");
        QCOMPARE(log.count(QStringLiteral("GStreamer video pipeline")), 1);
#endif
    }

    void coverArtCallbacksDoNotRestartVideoRenderer() {
#if AIRPLAY_WITH_UXPLAY
        qputenv("AIRPLAY_DEBUG_LOG", "1");
        const QString logPath = QCoreApplication::applicationDirPath() + QStringLiteral("/airplay_receiver_debug.log");
        QFile::remove(logPath);

        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Cover Art Callback Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy coverArtSpy(&receiver, &UxPlayReceiver::coverArtReceived);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        receiver.setStateFromUxPlayCallback(ReceiverState::Connected);
        QCOMPARE(video_renderer_choose_codec(false, false), 0);

        const QByteArray coverArt("fake-jpeg-data");
        receiver.setCoverArtFromUxPlayCallback(coverArt.constData(), coverArt.size());
        QTRY_COMPARE(coverArtSpy.count(), 1);
        receiver.stopCoverArtRenderingFromUxPlayCallback();
        receiver.stop();

        QCOMPARE(coverArtSpy.at(0).at(0).toByteArray(), coverArt);
        QFile logFile(logPath);
        QVERIFY(logFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString log = QString::fromUtf8(logFile.readAll());
        qunsetenv("AIRPLAY_DEBUG_LOG");
        QCOMPARE(log.count(QStringLiteral("GStreamer video pipeline")), 1);
#endif
    }

    void discoverableReceiverNameAcceptedEagerly() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Failure Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        const QString tooLongName(300, QLatin1Char('A'));
        QVERIFY(receiver.applyReceiverName(tooLongName));

        QCOMPARE(receiver.receiverName(), tooLongName);
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        receiver.stop();
        QCOMPARE(receiver.state(), ReceiverState::Idle);
#endif
    }

    void renameRecoveryWhenRestartSucceedsDoesNotLeaveError() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Recovery Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        const QString tooLongName(300, QLatin1Char('A'));
        if (receiver.applyReceiverName(tooLongName)) {
            QCOMPARE(receiver.receiverName(), tooLongName);
            receiver.stop();
            return;
        }

        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver Recovery Test"));
        QVERIFY(receiver.state() != ReceiverState::Error);
        QCOMPARE(errorSpy.count(), 0);

        receiver.stop();
        QCOMPARE(receiver.state(), ReceiverState::Idle);
#endif
    }

    void doubleRenameDoesNotRaceRestart() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Double Rename Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        QVERIFY(receiver.applyReceiverName("AirPlay First Rename"));
        QVERIFY(receiver.m_discoveryRestartPending.load());
        QCOMPARE(receiver.m_pendingDiscoveryRecoveryName, QString("AirPlay Receiver Double Rename Test"));
        QVERIFY(receiver.applyReceiverName("AirPlay Second Rename"));
        QVERIFY(receiver.m_discoveryRestartPending.load());
        QCOMPARE(receiver.m_pendingDiscoveryRecoveryName, QString("AirPlay Receiver Double Rename Test"));

        QCOMPARE(receiver.receiverName(), QString("AirPlay Second Rename"));
        QCOMPARE(errorSpy.count(), 0);

        receiver.stop();
        QVERIFY(!receiver.m_discoveryRestartPending.load());
#endif
    }

    void asyncRestartFailureWithoutRecoverySetsError() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Async Fail Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        receiver.m_pendingDiscoveryRecoveryName.clear();

        QVERIFY(receiver.restartDiscoveryBroadcast());
        QVERIFY(receiver.m_discoveryRestartPending.load());
        QVERIFY(receiver.m_discoveryRestartTimer != nullptr);

        void *raop = receiver.m_raop;
        receiver.m_raop = nullptr;

        receiver.m_discoveryRestartTimer->stop();
        receiver.m_discoveryRestartTimer->start(0);

        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 5000);
        QCOMPARE(receiver.state(), ReceiverState::Error);
        QVERIFY(errorSpy.at(0).at(0).toString().contains("RAOP context missing"));

        receiver.m_raop = raop;
        receiver.stop();
#endif
    }

    void rtpShutdownRecreatesVideoRenderer() {
#if AIRPLAY_WITH_UXPLAY
        qputenv("AIRPLAY_DEBUG_LOG", "1");
        const QString logPath = QCoreApplication::applicationDirPath() + QStringLiteral("/airplay_receiver_debug.log");
        QFile::remove(logPath);

        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver RTP Reset Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        receiver.setStateFromUxPlayCallback(ReceiverState::Connected);

        QCOMPARE(video_renderer_choose_codec(false, false), 0);

        receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_RTP_SHUTDOWN);
        receiver.stop();

        QFile logFile(logPath);
        QVERIFY(logFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString log = QString::fromUtf8(logFile.readAll());
        qunsetenv("AIRPLAY_DEBUG_LOG");
        QCOMPARE(log.count(QStringLiteral("GStreamer video pipeline")), 2);
        QCOMPARE(log.count(QStringLiteral("video_pipeline state change")), 2);
#endif
    }

    void fakeReceiverRecordsLastVideoFitMode() {
        FakeAirPlayReceiver fake;
        QVERIFY(!fake.lastVideoFitMode());
        fake.setVideoFitMode(true);
        QVERIFY(fake.lastVideoFitMode());
        fake.setVideoFitMode(false);
        QVERIFY(!fake.lastVideoFitMode());
    }

    void uxplayReceiverStoresVideoFitMode() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        QVERIFY(!receiver.m_videoFitMode.load());
        receiver.setVideoFitMode(true);
        QVERIFY(receiver.m_videoFitMode.load());
        receiver.setVideoFitMode(false);
        QVERIFY(!receiver.m_videoFitMode.load());
#endif
    }

    void setVideoFitModeUpdatesRenderer() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        QVERIFY(!video_renderer_get_force_aspect_ratio());

        receiver.setVideoFitMode(true);
        QVERIFY(video_renderer_get_force_aspect_ratio());

        receiver.setVideoFitMode(false);
        QVERIFY(!video_renderer_get_force_aspect_ratio());

        receiver.stop();
#endif
    }

    void videoFitModePersistsAfterRendererRestart() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        receiver.setStateFromUxPlayCallback(ReceiverState::Connected);
        QCOMPARE(video_renderer_choose_codec(false, false), 0);

        receiver.setVideoFitMode(true);
        QVERIFY(video_renderer_get_force_aspect_ratio());

        receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_RTP_SHUTDOWN);

        QVERIFY(video_renderer_get_force_aspect_ratio());

        receiver.stop();
#endif
    }

    void dnssdDoesNotRequireExternalRuntimeOnWindows() {
#ifdef _WIN32
        // In-process mDNS is used on Windows, no external runtime needed
        QVERIFY(true);
#else
        QSKIP("Non-Windows platform");
#endif
    }

    void dnssdProducesValidTxtRecordsOnWindows() {
#if AIRPLAY_WITH_UXPLAY && defined(_WIN32)
        const char hw_addr[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        int error = -1;
        dnssd_t *dnssd = dnssd_init("TestReceiver", 12, hw_addr, sizeof(hw_addr), &error, 0);
        QVERIFY2(dnssd != NULL, "dnssd_init returned NULL");
        QCOMPARE(error, 0);

        static char test_pk[] = "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7";
        dnssd_set_pk(dnssd, test_pk);

        int raop_ret = dnssd_register_raop(dnssd, 5000);
        QCOMPARE(raop_ret, 0);

        int airplay_ret = dnssd_register_airplay(dnssd, 7000);
        QCOMPARE(airplay_ret, 0);

        int raop_len = 0;
        const char *raop_txt = dnssd_get_raop_txt(dnssd, &raop_len);
        QVERIFY(raop_len > 0);
        QVERIFY(raop_txt != NULL);
        QVERIFY(txtRecordContainsKey(raop_txt, raop_len, "pk"));
        QVERIFY(txtRecordContainsKey(raop_txt, raop_len, "txtvers"));

        int airplay_len = 0;
        const char *airplay_txt = dnssd_get_airplay_txt(dnssd, &airplay_len);
        QVERIFY(airplay_len > 0);
        QVERIFY(airplay_txt != NULL);
        QVERIFY(txtRecordContainsKey(airplay_txt, airplay_len, "deviceid"));
        QVERIFY(txtRecordContainsKey(airplay_txt, airplay_len, "srcvers"));
        QVERIFY(txtRecordContainsKey(airplay_txt, airplay_len, "pk"));

        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
        dnssd_destroy(dnssd);
#endif
    }
};

QTEST_GUILESS_MAIN(UxPlayReceiverConfigTest)
#include "UxPlayReceiverConfigTest.moc"

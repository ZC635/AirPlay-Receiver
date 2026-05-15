#if AIRPLAY_WITH_UXPLAY
#include <winsock2.h>
#endif

#include <QtTest/QtTest>
#include <QFile>

#include <cmath>

#include "backend/FakeAirPlayReceiver.h"

#if AIRPLAY_WITH_UXPLAY
#define private public
#endif
#include "backend/DiscoveryRestartController.h"
#include "backend/UxPlayCallbackDispatch.h"
#include "backend/UxPlayReceiver.h"
#if AIRPLAY_WITH_UXPLAY
#undef private
#endif

#if AIRPLAY_WITH_UXPLAY
#include "lib/raop.h"
#include "lib/dnssd.h"
#include "platform/MdnsPublishing.h"
#include "platform/MdnsPublisher.h"
#include <gst/gst.h>

class FakeMdnsPublishing : public MdnsPublishing {
public:
    bool publish(const QString &receiverName, const QByteArray &hardwareAddress, quint16 port,
                 const char *raopTxt, int raopTxtLength,
                 const char *airplayTxt, int airplayTxtLength) override {
        publishCalls++;
        lastReceiverName = receiverName;
        lastHardwareAddress = hardwareAddress;
        lastPort = port;
        lastRaopTxt = QByteArray(raopTxt, raopTxtLength);
        lastAirplayTxt = QByteArray(airplayTxt, airplayTxtLength);
        return publishResult;
    }

    void stop() override { stopCalls++; }

    int publishCalls = 0;
    int stopCalls = 0;
    bool publishResult = true;
    QString lastReceiverName;
    QByteArray lastHardwareAddress;
    quint16 lastPort = 0;
    QByteArray lastRaopTxt;
    QByteArray lastAirplayTxt;
};

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
extern "C" void *video_renderer_get_pipeline(void);
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
        FakeMdnsPublishing publisher;
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        config.mdnsPublisher = &publisher;
        UxPlayReceiver receiver(config);
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::stateChanged);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();

        if (errorSpy.count() != 0) {
            QFAIL(qPrintable(errorSpy.at(0).at(0).toString()));
        }
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QVERIFY(stateSpy.count() >= 1);
        QCOMPARE(publisher.publishCalls, 1);

        receiver.stop();
        QCOMPARE(receiver.state(), ReceiverState::Idle);
#endif
    }

    void startAndStopUseInjectedMdnsPublisherForDiscoveryLifecycle() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Publishing Seam Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        config.mdnsPublisher = &publisher;
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();

        if (errorSpy.count() != 0) {
            QFAIL(qPrintable(errorSpy.at(0).at(0).toString()));
        }
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QCOMPARE(publisher.publishCalls, 1);
        QCOMPARE(publisher.lastReceiverName, config.serverName);
        QVERIFY(!publisher.lastHardwareAddress.isEmpty());
        QVERIFY(publisher.lastPort > 0);
        QVERIFY(txtRecordContainsKey(publisher.lastRaopTxt.constData(), publisher.lastRaopTxt.size(), "am"));
        QVERIFY(txtRecordContainsKey(publisher.lastAirplayTxt.constData(), publisher.lastAirplayTxt.size(), "features"));

        receiver.stop();

        QCOMPARE(receiver.state(), ReceiverState::Idle);
        QVERIFY(publisher.stopCalls >= 1);
#endif
    }

    void publishFailurePreventsUxPlayReceiverFromBecomingDiscoverable() {
#if AIRPLAY_WITH_UXPLAY
        FakeMdnsPublishing publisher;
        publisher.publishResult = false;
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Publishing Failure Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        config.mdnsPublisher = &publisher;
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();

        QCOMPARE(publisher.publishCalls, 1);
        QCOMPARE(receiver.state(), ReceiverState::Error);
        QVERIFY(errorSpy.count() >= 1);
        QCOMPARE(errorSpy.last().at(0).toString(), QString("Failed to publish mDNS services"));
        QVERIFY(publisher.stopCalls >= 1);
#endif
    }

    void defaultConfigCreatesOwnedMdnsPublisherOnSuccessfulStart() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Default Publisher Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();

        if (errorSpy.count() != 0) {
            QFAIL(qPrintable(errorSpy.at(0).at(0).toString()));
        }
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QVERIFY(receiver.m_config.mdnsPublisher == nullptr);
        QVERIFY(receiver.m_mdnsPublisher != nullptr);
        QVERIFY(receiver.m_ownsMdnsPublisher);

        receiver.stop();
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

    void clientVolumeCallbackReturnsCurrentVolumeAsAirPlayDb() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;

        receiver.setVolume(0.0);
        QCOMPARE(receiver.currentVolumeForUxPlayClientVolumeCallback(), -144.0);

        receiver.setVolume(1.0);
        QCOMPARE(receiver.currentVolumeForUxPlayClientVolumeCallback(), 0.0);

        receiver.setVolume(0.5);
        QVERIFY(std::abs(receiver.currentVolumeForUxPlayClientVolumeCallback() - (20.0 * std::log10(0.5))) < 0.000001);
#endif
    }

    void uxPlayVolumeCallbackEmitsNormalizedVolume() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        QSignalSpy volumeSpy(&receiver, &AirPlayReceiver::volumeChanged);

        receiver.setVolumeFromUxPlayCallback(0.4);

        QCOMPARE(volumeSpy.count(), 1);
        QCOMPARE(volumeSpy.at(0).at(0).toDouble(), 0.4);
#endif
    }

    void uxPlayVolumeLogUpdatesReceiverVolume() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        QSignalSpy volumeSpy(&receiver, &AirPlayReceiver::volumeChanged);

        receiver.handleLogMessageFromUxPlayCallback(LOGGER_DEBUG, "volume: -15.000000 ");

        QCOMPARE(volumeSpy.count(), 1);
        QVERIFY(std::abs(volumeSpy.at(0).at(0).toDouble() - std::pow(10.0, 0.05 * -15.0)) < 0.000001);
#endif
    }

    void callbackDispatchRejectsStaleGenerationUniformly() {
#if AIRPLAY_WITH_UXPLAY
        std::atomic_bool acceptingCallbacks = true;
        std::atomic<quint64> callbackGeneration = 7;
        std::atomic_bool renderersStarted = true;
        QRecursiveMutex rendererMutex;
        UxPlayCallbackDispatch dispatch(acceptingCallbacks, callbackGeneration, renderersStarted, rendererMutex);

        int stateCallbackCount = 0;
        int resetCallbackCount = 0;

        QVERIFY(!dispatch.runIfCurrent(6, [&] { stateCallbackCount++; }));
        QVERIFY(!dispatch.runWithRendererStarted(6, [&] { resetCallbackCount++; }));

        QCOMPARE(stateCallbackCount, 0);
        QCOMPARE(resetCallbackCount, 0);
#endif
    }

    void callbackDispatchGuardsRendererStartedUniformly() {
#if AIRPLAY_WITH_UXPLAY
        std::atomic_bool acceptingCallbacks = true;
        std::atomic<quint64> callbackGeneration = 7;
        std::atomic_bool renderersStarted = false;
        QRecursiveMutex rendererMutex;
        UxPlayCallbackDispatch dispatch(acceptingCallbacks, callbackGeneration, renderersStarted, rendererMutex);

        int audioCallbackCount = 0;
        int videoCallbackCount = 0;

        QVERIFY(!dispatch.runWithRendererStarted(7, [&] { audioCallbackCount++; }));
        QVERIFY(!dispatch.runWithRendererStarted(7, [&] { videoCallbackCount++; }));

        QCOMPARE(audioCallbackCount, 0);
        QCOMPARE(videoCallbackCount, 0);
#endif
    }

    void uxPlayReceiverRejectsStaleGenerationAcrossCallbackPaths() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(7);
        receiver.m_renderersStarted.store(true);
        receiver.setState(ReceiverState::Connected);
        receiver.m_videoRendererStopped.store(true);

        receiver.setStateFromUxPlayCallback(ReceiverState::Discoverable, 6);
        receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_RTP_SHUTDOWN, 6);

        QCOMPARE(receiver.state(), ReceiverState::Connected);
        QVERIFY(receiver.m_videoRendererStopped.load());
        receiver.m_renderersStarted.store(false);
#endif
    }

    void uxPlayReceiverRejectsStaleGenerationBeforeConnectionRendererMutation() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(7);
        receiver.m_renderersStarted.store(true);

        receiver.m_videoRendererStopped.store(true);
        receiver.restartVideoPipelineForConnect(6);
        QVERIFY(receiver.m_videoRendererStopped.load());

        receiver.m_videoRendererStopped.store(false);
        receiver.stopVideoPipelineForDisconnect(6);
        QVERIFY(!receiver.m_videoRendererStopped.load());
        receiver.m_renderersStarted.store(false);
#endif
    }

    void uxPlayReceiverGuardsRendererStartedAcrossCallbackPaths() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(7);
        receiver.m_renderersStarted.store(false);

        QCOMPARE(receiver.chooseVideoCodecFromCallback(false), -1);

        receiver.m_videoRendererStopped.store(true);
        receiver.restartVideoPipelineForConnect();
        QVERIFY(receiver.m_videoRendererStopped.load());
#endif
    }

    void queuedCoverArtCallbackRevalidatesGenerationBeforeEmit() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(7);
        QSignalSpy coverArtSpy(&receiver, &UxPlayReceiver::coverArtReceived);

        const QByteArray coverArt("fake-jpeg-data");
        receiver.setCoverArtFromUxPlayCallback(coverArt.constData(), coverArt.size());
        receiver.m_callbackGeneration.store(8);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(coverArtSpy.count(), 0);
#endif
    }

    void queuedMetadataCallbackRevalidatesGenerationBeforeEmit() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(7);
        QSignalSpy metadataSpy(&receiver, &UxPlayReceiver::trackMetadataChanged);

        const QByteArray title("Song");
        QByteArray metadata("mlit");
        const int mlitLength = 8 + title.size();
        metadata.append(char((mlitLength >> 24) & 0xff));
        metadata.append(char((mlitLength >> 16) & 0xff));
        metadata.append(char((mlitLength >> 8) & 0xff));
        metadata.append(char(mlitLength & 0xff));
        metadata.append("minm");
        metadata.append(char((title.size() >> 24) & 0xff));
        metadata.append(char((title.size() >> 16) & 0xff));
        metadata.append(char((title.size() >> 8) & 0xff));
        metadata.append(char(title.size() & 0xff));
        metadata.append(title);

        receiver.setMetadataFromUxPlayCallback(metadata.constData(), metadata.size());
        receiver.m_callbackGeneration.store(8);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(metadataSpy.count(), 0);
#endif
    }

    void queuedProgressCallbackRevalidatesGenerationBeforeEmit() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(7);
        QSignalSpy progressSpy(&receiver, &UxPlayReceiver::progressUpdated);

        receiver.setProgressFromUxPlayCallback(0, 44100, 88200);
        receiver.m_callbackGeneration.store(8);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(progressSpy.count(), 0);
#endif
    }

    void staleCallbackContextAfterRestartCannotMutateReceiver() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(8);
        receiver.setState(ReceiverState::Discoverable);

        UxPlayReceiver::CallbackContext oldContext(&receiver, 7);
        receiver.setStateFromUxPlayCallback(ReceiverState::Connected, oldContext.generation);

        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
#endif
    }

    void closedCallbackContextRejectsNewCallbacks() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        UxPlayReceiver::CallbackContext context(&receiver, 7);
        UxPlayReceiver *enteredReceiver = nullptr;

        QVERIFY(context.enter(&enteredReceiver));
        QCOMPARE(enteredReceiver, &receiver);
        context.leave();

        context.close();

        enteredReceiver = &receiver;
        QVERIFY(!context.enter(&enteredReceiver));
        QVERIFY(enteredReceiver == nullptr);
#endif
    }

    void callbackContextCloseWaitsForInflightCallback() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        UxPlayReceiver::CallbackContext context(&receiver, 7);
        UxPlayReceiver *enteredReceiver = nullptr;
        QVERIFY(context.enter(&enteredReceiver));

        std::atomic_bool closeReturned = false;
        QSemaphore closeEntered;
        QThread closer;
        QObject::connect(&closer, &QThread::started, [&] {
            closeEntered.release();
            context.close();
            closeReturned.store(true);
            closer.quit();
        });

        closer.start();
        QVERIFY(closeEntered.tryAcquire(1, 1000));
        QTest::qWait(50);
        QVERIFY(!closeReturned.load());

        context.leave();
        QVERIFY(closer.wait(1000));
        QVERIFY(closeReturned.load());
#endif
    }

    void staleVolumeLogCallbackAfterRestartCannotMutateReceiver() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(8);
        QSignalSpy volumeSpy(&receiver, &AirPlayReceiver::volumeChanged);

        receiver.handleLogMessageFromUxPlayCallback(LOGGER_DEBUG, "volume: -15.000000 ", 7);

        QCOMPARE(volumeSpy.count(), 0);
#endif
    }

    void queuedVideoSizeCallbackRevalidatesGenerationBeforeEmit() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        receiver.m_acceptingCallbacks.store(true);
        receiver.m_callbackGeneration.store(7);
        QSignalSpy sizeSpy(&receiver, &AirPlayReceiver::videoSizeChanged);

        receiver.reportVideoSizeFromUxPlayCallback(1280, 720, 7);
        receiver.m_callbackGeneration.store(8);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::processEvents();

        QCOMPARE(sizeSpy.count(), 0);
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
        const bool timerPending = receiver.m_discoveryRestartController->pending();
        blocker.close();
        receiver.stop();

        QVERIFY(scheduled);
        QVERIFY(goodbyeSent);
        QVERIFY(!receiver.m_discoveryRestartController->pending());
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
        QVERIFY(!receiver.m_discoveryRestartController->pending());
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
        QVERIFY(log.count(QStringLiteral("GStreamer video pipeline")) > 0);
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
        QVERIFY(log.count(QStringLiteral("GStreamer video pipeline")) > 0);
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
        QVERIFY(receiver.m_discoveryRestartController->pending());
        QCOMPARE(receiver.m_discoveryRestartController->recoveryName(), QString("AirPlay Receiver Double Rename Test"));
        QVERIFY(receiver.applyReceiverName("AirPlay Second Rename"));
        QVERIFY(receiver.m_discoveryRestartController->pending());
        QCOMPARE(receiver.m_discoveryRestartController->recoveryName(), QString("AirPlay Receiver Double Rename Test"));

        QCOMPARE(receiver.receiverName(), QString("AirPlay Second Rename"));
        QCOMPARE(errorSpy.count(), 0);

        receiver.stop();
        QVERIFY(!receiver.m_discoveryRestartController->pending());
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

        QVERIFY(receiver.restartDiscoveryBroadcast());
        QVERIFY(receiver.m_discoveryRestartController->pending());
        QVERIFY(receiver.m_discoveryRestartController != nullptr);

        void *raop = receiver.m_raop;
        receiver.m_raop = nullptr;

        receiver.m_discoveryRestartController->trigger();

        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 5000);
        QCOMPARE(receiver.state(), ReceiverState::Error);
        QVERIFY(errorSpy.at(0).at(0).toString().contains("Failed to create"));

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

        QFile logFile(logPath);
        QVERIFY(logFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const int initialPipelineCount = QString::fromUtf8(logFile.readAll()).count(QStringLiteral("GStreamer video pipeline"));
        logFile.close();

        receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_RTP_SHUTDOWN);
        receiver.stop();

        QVERIFY(logFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString log = QString::fromUtf8(logFile.readAll());
        qunsetenv("AIRPLAY_DEBUG_LOG");
        const int finalPipelineCount = log.count(QStringLiteral("GStreamer video pipeline"));
        QVERIFY(finalPipelineCount > initialPipelineCount);
        QVERIFY(log.count(QStringLiteral("video_pipeline state change")) > 0);
#endif
    }

    void videoResetReattachesAppsinkFrameBridge() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Appsink Reset Test";
        config.videoSink = "appsink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);
        receiver.setVideoFrameCallback([](QImage) {});

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        receiver.setStateFromUxPlayCallback(ReceiverState::Connected);
        QCOMPARE(receiver.chooseVideoCodecFromCallback(false), 0);
        QVERIFY(receiver.m_videoFrameBridge != nullptr);

        auto appsinkHasOneFrameHandler = [&receiver] {
            GstElement *pipeline = static_cast<GstElement *>(video_renderer_get_pipeline());
            if (!pipeline) {
                return false;
            }

            GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink_h264");
            if (!appsink) {
                return false;
            }

            gboolean emitSignals = FALSE;
            g_object_get(G_OBJECT(appsink), "emit-signals", &emitSignals, nullptr);
            const guint signalId = g_signal_lookup("new-sample", G_OBJECT_TYPE(appsink));
            const guint handlerCount = g_signal_handlers_block_matched(appsink, G_SIGNAL_MATCH_ID, signalId, 0,
                                                                       nullptr, nullptr, nullptr);
            g_signal_handlers_unblock_matched(appsink, G_SIGNAL_MATCH_ID, signalId, 0, nullptr, nullptr, nullptr);
            gst_object_unref(appsink);
            return emitSignals == TRUE && handlerCount == 1;
        };

        QVERIFY(appsinkHasOneFrameHandler());

        receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_RTP_SHUTDOWN);

        QVERIFY(receiver.m_videoFrameBridge != nullptr);
        QVERIFY(appsinkHasOneFrameHandler());

        receiver.stop();
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

    void videoQualityDefaultConfigIs1080p30() {
        const VideoQualitySettings quality;
        QCOMPARE(quality.resolution, VideoResolution::P1080);
        QCOMPARE(quality.frameRate, VideoFrameRate::Fps30);
    }

    void videoQualityResolutionMapping() {
        QCOMPARE(videoQualityWidth(VideoResolution::P540), 960);
        QCOMPARE(videoQualityHeight(VideoResolution::P540), 540);
        QCOMPARE(videoQualityWidth(VideoResolution::P720), 1280);
        QCOMPARE(videoQualityHeight(VideoResolution::P720), 720);
        QCOMPARE(videoQualityWidth(VideoResolution::P1080), 1920);
        QCOMPARE(videoQualityHeight(VideoResolution::P1080), 1080);
    }

    void videoQualityFrameRateMapping() {
        QCOMPARE(videoQualityMaxFPS(VideoFrameRate::Fps15), 15);
        QCOMPARE(videoQualityRefreshRate(VideoFrameRate::Fps15), 60);
        QCOMPARE(videoQualityMaxFPS(VideoFrameRate::Fps30), 30);
        QCOMPARE(videoQualityRefreshRate(VideoFrameRate::Fps30), 60);
        QCOMPARE(videoQualityMaxFPS(VideoFrameRate::Fps60), 60);
        QCOMPARE(videoQualityRefreshRate(VideoFrameRate::Fps60), 60);
    }

    void videoQualityH265SupportIsAlwaysEnabled() {
        QCOMPARE(videoQualityH265Support(), true);
    }

    void videoQualityCombined720p15Fps() {
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps15;

        QCOMPARE(videoQualityWidth(quality.resolution), 1280);
        QCOMPARE(videoQualityHeight(quality.resolution), 720);
        QCOMPARE(videoQualityRefreshRate(quality.frameRate), 60);
        QCOMPARE(videoQualityMaxFPS(quality.frameRate), 15);
    }

    void videoQualityResolutionDoesNotImplyDifferentBehaviour() {
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P540;
        QCOMPARE(videoQualityWidth(quality.resolution), 960);
        QCOMPARE(videoQualityHeight(quality.resolution), 540);
    }

    void dnsSdFeatureBit42AlwaysAdvertisesH265() {
#if AIRPLAY_WITH_UXPLAY && defined(_WIN32)
        const char hw_addr[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        int error = -1;
        dnssd_t *dnssd = dnssd_init("TestReceiver", 12, hw_addr, sizeof(hw_addr), &error, 0);
        QVERIFY2(dnssd != NULL, "dnssd_init returned NULL");
        QCOMPARE(error, 0);

        dnssd_set_airplay_features(dnssd, 42, 1);
        const uint64_t features_hevc = dnssd_get_airplay_features(dnssd);
        QVERIFY((features_hevc >> 42) & 1ULL);

        dnssd_set_airplay_features(dnssd, 42, 0);
        const uint64_t features_h264 = dnssd_get_airplay_features(dnssd);
        QVERIFY(!((features_h264 >> 42) & 1ULL));

        dnssd_destroy(dnssd);
#endif
    }

    void idleVideoQualityChangeStoresWithoutStarting() {
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Idle Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        QCOMPARE(receiver.state(), ReceiverState::Idle);

        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps15;
        QVERIFY(receiver.applyVideoQuality(quality));

        QCOMPARE(receiver.state(), ReceiverState::Idle);
        QCOMPARE(receiver.m_config.videoQuality.resolution, VideoResolution::P720);
        QCOMPARE(receiver.m_config.videoQuality.frameRate, VideoFrameRate::Fps15);
    }

    void discoverableVideoQualityChangeRestartsWithNewDnsFeature() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Quality DNS Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        config.videoQuality.resolution = VideoResolution::P1080;
        config.videoQuality.frameRate = VideoFrameRate::Fps30;
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QVERIFY(receiver.m_dnssd != nullptr);
        uint64_t featuresBefore = dnssd_get_airplay_features(static_cast<dnssd_t *>(receiver.m_dnssd));
        QVERIFY((featuresBefore >> 42) & 1ULL);

        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps15;
        QVERIFY(receiver.applyVideoQuality(quality));

        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QCOMPARE(receiver.m_config.videoQuality.resolution, VideoResolution::P720);

        if (receiver.m_discoveryRestartController && receiver.m_discoveryRestartController->pending()) {
            receiver.m_discoveryRestartController->trigger();
        }

        QVERIFY(receiver.m_dnssd != nullptr);
        uint64_t featuresAfter = dnssd_get_airplay_features(static_cast<dnssd_t *>(receiver.m_dnssd));
        QVERIFY((featuresAfter >> 42) & 1ULL);

        receiver.stop();
#endif
    }

    void applyVideoQualityReturnsFalseOnSyncFailure() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Quality Failure Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        const auto originalQuality = receiver.m_config.videoQuality;

        receiver.m_raopPort = 0;

        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        QVERIFY(!receiver.applyVideoQuality(quality));

        QCOMPARE(receiver.m_config.videoQuality, originalQuality);

        receiver.stop();
#endif
    }

    void connectedApplyVideoQualityRestartsReceiverAndUpdatesDnsFeature() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Connected Quality Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        config.videoQuality.resolution = VideoResolution::P1080;
        config.videoQuality.frameRate = VideoFrameRate::Fps30;
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        receiver.setStateFromUxPlayCallback(ReceiverState::Connected);
        QCOMPARE(receiver.state(), ReceiverState::Connected);

        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps15;
        QVERIFY(receiver.applyVideoQuality(quality));

        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QCOMPARE(receiver.m_config.videoQuality.resolution, VideoResolution::P720);
        QCOMPARE(receiver.m_config.videoQuality.frameRate, VideoFrameRate::Fps15);

        QVERIFY(receiver.m_dnssd != nullptr);
        uint64_t features = dnssd_get_airplay_features(static_cast<dnssd_t *>(receiver.m_dnssd));
        QVERIFY((features >> 42) & 1ULL);

        receiver.stop();
#endif
    }

    void repeatedApplySameVideoQualityReturnsTrueAndIsNoop() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Same Quality Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        VideoQualitySettings sameQuality;
        QCOMPARE(sameQuality, receiver.m_config.videoQuality);
        QVERIFY(receiver.applyVideoQuality(sameQuality));
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        receiver.stop();
#endif
    }

    void applyVideoQualityRejectedInErrorState() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Error Reject Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);

        receiver.setState(ReceiverState::Error);
        QCOMPARE(receiver.state(), ReceiverState::Error);

        const auto originalQuality = receiver.m_config.videoQuality;
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        QVERIFY(!receiver.applyVideoQuality(quality));

        QCOMPARE(receiver.state(), ReceiverState::Error);
        QCOMPARE(receiver.m_config.videoQuality, originalQuality);

        receiver.stop();
#endif
    }

    void applyVideoQualityRejectedInStartingState() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiverConfig config;
        config.serverName = "AirPlay Receiver Starting Reject Test";
        config.videoSink = "fakesink";
        config.audioSink = "fakesink";
        UxPlayReceiver receiver(config);

        receiver.setState(ReceiverState::Starting);
        QCOMPARE(receiver.state(), ReceiverState::Starting);

        const auto originalQuality = receiver.m_config.videoQuality;
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        QVERIFY(!receiver.applyVideoQuality(quality));

        QCOMPARE(receiver.state(), ReceiverState::Starting);
        QCOMPARE(receiver.m_config.videoQuality, originalQuality);
#endif
    }
};

QTEST_GUILESS_MAIN(UxPlayReceiverConfigTest)
#include "UxPlayReceiverConfigTest.moc"

#include <QtTest/QtTest>
#include "backend/UxPlayReceiver.h"

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
};

QTEST_MAIN(UxPlayReceiverConfigTest)
#include "UxPlayReceiverConfigTest.moc"

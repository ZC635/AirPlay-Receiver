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

    void startWithUxPlayPlaceholderReportsNotImplemented() {
#if AIRPLAY_WITH_UXPLAY
        UxPlayReceiver receiver;
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::stateChanged);
        QSignalSpy errorSpy(&receiver, &AirPlayReceiver::errorChanged);

        receiver.start();

        QCOMPARE(receiver.state(), ReceiverState::Error);
        QCOMPARE(stateSpy.count(), 1);
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(errorSpy.at(0).at(0).toString(), QString("UxPlay receiver lifecycle is not implemented in this build"));
#endif
    }
};

QTEST_MAIN(UxPlayReceiverConfigTest)
#include "UxPlayReceiverConfigTest.moc"

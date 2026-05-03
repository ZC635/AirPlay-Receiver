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
};

QTEST_MAIN(UxPlayReceiverConfigTest)
#include "UxPlayReceiverConfigTest.moc"

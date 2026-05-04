#include <QtTest/QtTest>
#include <QFile>
#include "backend/UxPlayReceiver.h"

#if AIRPLAY_WITH_UXPLAY
#include "lib/raop.h"

extern "C" int video_renderer_choose_codec(bool video_is_jpeg, bool video_is_h265);
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
};

QTEST_MAIN(UxPlayReceiverConfigTest)
#include "UxPlayReceiverConfigTest.moc"

#include <QtTest/QtTest>
#include "backend/FakeAirPlayReceiver.h"

class AirPlayReceiverTest : public QObject {
    Q_OBJECT

private slots:
    void emitsStateChanges() {
        FakeAirPlayReceiver receiver;
        QSignalSpy spy(&receiver, &AirPlayReceiver::stateChanged);
        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QCOMPARE(spy.count(), 1);
    }

    void emitsVideoSizeChanged() {
        FakeAirPlayReceiver receiver;
        QSignalSpy spy(&receiver, &AirPlayReceiver::videoSizeChanged);
        receiver.emitVideoSize(1170, 2532);
        QCOMPARE(spy.count(), 1);
        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toInt(), 1170);
        QCOMPARE(args.at(1).toInt(), 2532);
    }
};

QTEST_MAIN(AirPlayReceiverTest)
#include "AirPlayReceiverTest.moc"

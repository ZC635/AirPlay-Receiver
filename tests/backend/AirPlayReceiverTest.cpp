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
};

QTEST_MAIN(AirPlayReceiverTest)
#include "AirPlayReceiverTest.moc"

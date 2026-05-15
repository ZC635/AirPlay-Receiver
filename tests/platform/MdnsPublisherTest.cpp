#include <QtTest/QtTest>
#include <QMap>
#include <QByteArray>

#include "platform/MdnsPublishing.h"
#include "platform/MdnsPublisher.h"

class MdnsPublisherTest : public QObject {
    Q_OBJECT

private slots:
    void parseTxtRecordParsesStandardKeyValuePairs() {
        const unsigned char raw[] = {
            9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1',
            4, 'c', 'h', '=', '2',
            5, 'a', 'm', '=', 'A', 'P',
            6, 'e', 't', '=', '0', ',', '2',
            2, 's', 'm',
            7, 'f', 'a', 'l', 's', 'e', '=', '1',
        };
        const int length = static_cast<int>(sizeof(raw));

        bool ok = false;
        QMap<QByteArray, QByteArray> result = MdnsPublisher::parseTxtRecord(
            reinterpret_cast<const char *>(raw), length, &ok);

        QVERIFY(ok);
        QCOMPARE(result.size(), 6);
        QCOMPARE(result.value(QByteArray("txtvers")), QByteArray("1"));
        QCOMPARE(result.value(QByteArray("ch")), QByteArray("2"));
        QCOMPARE(result.value(QByteArray("am")), QByteArray("AP"));
        QCOMPARE(result.value(QByteArray("et")), QByteArray("0,2"));
        QCOMPARE(result.value(QByteArray("false")), QByteArray("1"));
        QVERIFY(result.contains(QByteArray("sm")));
        QVERIFY(result.value(QByteArray("sm")).isEmpty());
    }

    void parseTxtRecordReturnsEmptyMapForNullInput() {
        bool ok = false;
        QMap<QByteArray, QByteArray> result = MdnsPublisher::parseTxtRecord(nullptr, 0, &ok);
        QVERIFY(result.isEmpty());
        QVERIFY(!ok);
    }

    void parseTxtRecordReturnsEmptyMapForZeroLength() {
        bool ok = false;
        QMap<QByteArray, QByteArray> result = MdnsPublisher::parseTxtRecord("", 0, &ok);
        QVERIFY(result.isEmpty());
        QVERIFY(!ok);
    }

    void parseTxtRecordTruncatesOnMalformedLength() {
        const unsigned char raw[] = {
            9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1',
            4, 'c', 'h', '=', '2',
            20, 'b', 'a', 'd', // length=20 but only 4 bytes follow
        };
        const int length = static_cast<int>(sizeof(raw));

        bool ok = true;
        QMap<QByteArray, QByteArray> result = MdnsPublisher::parseTxtRecord(
            reinterpret_cast<const char *>(raw), length, &ok);

        QVERIFY(ok);
        QCOMPARE(result.value(QByteArray("txtvers")), QByteArray("1"));
        QCOMPARE(result.value(QByteArray("ch")), QByteArray("2"));
        QVERIFY(!result.contains(QByteArray("bad")));
    }

    void buildServiceDefinitionsCreatesRaopService() {
        const QByteArray hwAddr = QByteArray::fromHex("020000000001");
        const unsigned char raopRaw[] = {
            9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1',
            7, 'c', 'n', '=', '0', ',', '1', ',',
        };
        const unsigned char airplayRaw[] = {
            14, 'd', 'e', 'v', 'i', 'c', 'e', 'i', 'd', '=', 'E', '8', ':', '8', '8',
        };

        bool ok = false;
        QList<MdnsPublisher::ServiceDefinition> defs = MdnsPublisher::buildServiceDefinitions(
            QStringLiteral("AirPlay Receiver"), hwAddr, 5000,
            reinterpret_cast<const char *>(raopRaw), static_cast<int>(sizeof(raopRaw)),
            reinterpret_cast<const char *>(airplayRaw), static_cast<int>(sizeof(airplayRaw)),
            &ok);

        QVERIFY(ok);
        QCOMPARE(defs.size(), 2);

        const auto &raop = defs.at(0);
        QCOMPARE(raop.type, QByteArray("_raop._tcp.local."));
        QCOMPARE(raop.name, QByteArray("020000000001@AirPlay Receiver"));
        QCOMPARE(raop.port, static_cast<quint16>(5000));
        QCOMPARE(raop.attributes.value(QByteArray("txtvers")), QByteArray("1"));
        QCOMPARE(raop.attributes.value(QByteArray("cn")), QByteArray("0,1,"));

        const auto &airplay = defs.at(1);
        QCOMPARE(airplay.type, QByteArray("_airplay._tcp.local."));
        QCOMPARE(airplay.name, QByteArray("AirPlay Receiver"));
        QCOMPARE(airplay.port, static_cast<quint16>(5000));
        QCOMPARE(airplay.attributes.value(QByteArray("deviceid")), QByteArray("E8:88"));
    }

    void buildServiceDefinitionsReturnsEmptyListForBadRaopTxt() {
        const QByteArray hwAddr = QByteArray::fromHex("020000000001");
        const unsigned char airplayRaw[] = {
            13, 'd', 'e', 'v', 'i', 'c', 'e', 'i', 'd', '=', 't', 'e', 's', 't',
        };

        bool ok = true;
        QList<MdnsPublisher::ServiceDefinition> defs = MdnsPublisher::buildServiceDefinitions(
            QStringLiteral("Test"), hwAddr, 7000,
            nullptr, 0,
            reinterpret_cast<const char *>(airplayRaw), static_cast<int>(sizeof(airplayRaw)),
            &ok);

        QVERIFY(!ok);
        QVERIFY(defs.isEmpty());
    }

    void buildServiceDefinitionsReturnsEmptyListForBadAirplayTxt() {
        const QByteArray hwAddr = QByteArray::fromHex("020000000001");
        const unsigned char raopRaw[] = {
            9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1',
        };

        bool ok = true;
        QList<MdnsPublisher::ServiceDefinition> defs = MdnsPublisher::buildServiceDefinitions(
            QStringLiteral("Test"), hwAddr, 7000,
            reinterpret_cast<const char *>(raopRaw), static_cast<int>(sizeof(raopRaw)),
            nullptr, 0,
            &ok);

        QVERIFY(!ok);
        QVERIFY(defs.isEmpty());
    }

    void raopNameUsesUppercaseHardwareAddressHex() {
        const QByteArray hwAddr = QByteArray::fromHex("abCDef012345");

        const unsigned char raw[] = {
            9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1',
        };

        bool ok = false;
        QList<MdnsPublisher::ServiceDefinition> defs = MdnsPublisher::buildServiceDefinitions(
            QStringLiteral("My Receiver"), hwAddr, 7000,
            reinterpret_cast<const char *>(raw), static_cast<int>(sizeof(raw)),
            reinterpret_cast<const char *>(raw), static_cast<int>(sizeof(raw)),
            &ok);

        QVERIFY(ok);
        QCOMPARE(defs.size(), 2);
        QCOMPARE(defs.at(0).name, QByteArray("ABCDEF012345@My Receiver"));
    }

    void mdnsPublisherImplementsPublishingInterface() {
        MdnsPublisher publisher;
        MdnsPublishing *publishing = &publisher;

        QVERIFY(publishing != nullptr);
    }

#if !AIRPLAY_WITH_UXPLAY
    void publishReturnsFalseInNonUxPlayBuild() {
        MdnsPublisher publisher;
        const QByteArray hwAddr = QByteArray::fromHex("020000000001");
        const unsigned char raopRaw[] = {
            9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1',
        };
        const unsigned char airplayRaw[] = {
            14, 'd', 'e', 'v', 'i', 'c', 'e', 'i', 'd', '=', 'E', '8', ':', '8', '8',
        };

        bool result = publisher.publish(
            QStringLiteral("Test Receiver"), hwAddr, 5000,
            reinterpret_cast<const char *>(raopRaw), static_cast<int>(sizeof(raopRaw)),
            reinterpret_cast<const char *>(airplayRaw), static_cast<int>(sizeof(airplayRaw)));

        QVERIFY(!result);
    }
#endif
};

QTEST_GUILESS_MAIN(MdnsPublisherTest)
#include "MdnsPublisherTest.moc"

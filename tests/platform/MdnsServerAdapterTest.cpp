#include <QtTest/QtTest>

#include "diagnostics/DiagnosticSanitizer.h"
#include "support/CollectingDiagnosticLogSink.h"

#if AIRPLAY_WITH_UXPLAY
#include <QHostAddress>
#include <QSignalSpy>
#include <qmdnsengine/abstractserver.h>
#include <qmdnsengine/dns.h>
#include <qmdnsengine/mdns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/query.h>
#include <qmdnsengine/record.h>

#include "platform/MdnsServerAdapter.h"

class FakeAbstractServer final : public QMdnsEngine::AbstractServer {
public:
    void sendMessage(const QMdnsEngine::Message &) override { ++sendMessageCalls; }
    void sendMessageToAll(const QMdnsEngine::Message &) override { ++sendToAllCalls; }
    void deliver(const QMdnsEngine::Message &message) { emit messageReceived(message); }
    void fail(const QString &message) { emit error(message); }

    int sendMessageCalls = 0;
    int sendToAllCalls = 0;
};
#endif

class MdnsServerAdapterTest : public QObject {
    Q_OBJECT

private slots:
#if AIRPLAY_WITH_UXPLAY
    void relevantQueryIsCountedWithoutPacketData() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QMdnsEngine::Message message;
        message.setAddress(QHostAddress("192.168.1.77"));
        QMdnsEngine::Query query;
        query.setName("_airplay._tcp.local.");
        query.setType(QMdnsEngine::PTR);
        message.addQuery(query);

        QSignalSpy forwarded(&adapter, &QMdnsEngine::AbstractServer::messageReceived);
        delegate.deliver(message);

        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(countEvents(sink, "search_received"), 1);
        const DiagnosticEvent event = findEvent(sink, "search_received");
        QCOMPARE(event.fields.value("query"), QString("_airplay._tcp"));
        QVERIFY(!event.fields.value("source").contains(".77"));
        QVERIFY(!event.fields.contains("packet"));
    }

    void irrelevantQueryIsForwardedWithoutRecording() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QMdnsEngine::Message message;
        QMdnsEngine::Query query;
        query.setName("_googlecast._tcp.local.");
        query.setType(QMdnsEngine::PTR);
        message.addQuery(query);

        QSignalSpy forwarded(&adapter, &QMdnsEngine::AbstractServer::messageReceived);
        delegate.deliver(message);

        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(countEvents(sink, "search_received"), 0);
    }

    void raopAndMixedCaseQueriesAreRecorded() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QMdnsEngine::Message message;
        QMdnsEngine::Query query;
        query.setName("_RaOp._TcP.LoCaL.");
        query.setType(QMdnsEngine::SRV);
        message.addQuery(query);

        delegate.deliver(message);

        const DiagnosticEvent event = findEvent(sink, "search_received");
        QCOMPARE(event.fields.value("query"), QString("_raop._tcp"));
        QCOMPARE(event.fields.value("type"), QString("SRV"));
    }

    void nearMissQueryIsForwardedWithoutRecording() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QMdnsEngine::Message message;
        QMdnsEngine::Query query;
        query.setName("_airplay._tcp.local.example.");
        query.setType(QMdnsEngine::PTR);
        message.addQuery(query);

        QSignalSpy forwarded(&adapter, &QMdnsEngine::AbstractServer::messageReceived);
        delegate.deliver(message);

        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(countEvents(sink, "search_received"), 0);
    }

    void queryTypesUseStableLabels() {
        const QList<QPair<quint16, QString>> cases = {
            {QMdnsEngine::PTR, QStringLiteral("PTR")}, {QMdnsEngine::SRV, QStringLiteral("SRV")},
            {QMdnsEngine::TXT, QStringLiteral("TXT")}, {QMdnsEngine::A, QStringLiteral("A")},
            {QMdnsEngine::AAAA, QStringLiteral("AAAA")}, {999, QStringLiteral("other")},
        };
        for (const auto &testCase : cases) {
            FakeAbstractServer delegate;
            CollectingSink sink;
            MdnsServerAdapter adapter(&delegate, &sink);
            QMdnsEngine::Message message;
            QMdnsEngine::Query query;
            query.setName("_airplay._tcp.local.");
            query.setType(testCase.first);
            message.addQuery(query);

            delegate.deliver(message);

            QCOMPARE(findEvent(sink, "search_received").fields.value("type"), testCase.second);
        }
    }

    void sourcesAreMaskedOrClassifiedByAddressFamily() {
        const QList<QPair<QHostAddress, QString>> cases = {
            {QHostAddress("192.168.1.77"), QStringLiteral("192.168.1.xxx/24")},
            {QHostAddress("10.2.3.4"), QStringLiteral("10.2.3.xxx/24")},
            {QHostAddress("fe80::1"), QStringLiteral("fe80:0000:0000:0000:xxxx/64")},
            {QHostAddress("8.8.8.8"), QStringLiteral("public_ipv4")},
            {QHostAddress("2001:4860:4860::8888"), QStringLiteral("public_ipv6")},
        };
        for (const auto &testCase : cases) {
            FakeAbstractServer delegate;
            CollectingSink sink;
            MdnsServerAdapter adapter(&delegate, &sink);
            QMdnsEngine::Message message;
            message.setAddress(testCase.first);
            QMdnsEngine::Query query;
            query.setName("_airplay._tcp.local.");
            message.addQuery(query);

            delegate.deliver(message);

            QCOMPARE(findEvent(sink, "search_received").fields.value("source"), testCase.second);
        }
    }

    void responseWithoutQueriesIsNotCountedAsSearch() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QMdnsEngine::Message response;
        response.setResponse(true);
        QMdnsEngine::Record answer;
        answer.setType(QMdnsEngine::A);
        response.addRecord(answer);

        delegate.deliver(response);

        QCOMPARE(countEvents(sink, "search_received"), 0);
    }

    void sendCallsAreRequestsNotSuccessClaims() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QMdnsEngine::Message message;
        message.setAddress(QMdnsEngine::MdnsIpv4Address);

        adapter.sendMessage(message);
        adapter.sendMessageToAll(message);

        QCOMPARE(delegate.sendMessageCalls, 1);
        QCOMPARE(delegate.sendToAllCalls, 1);
        QCOMPARE(countEvents(sink, "send_requested"), 2);
        for (const DiagnosticEvent &event : sink.events)
            QVERIFY(event.name != "send_succeeded");
    }

    void sendRequestsHaveExactScopeAndAddressFamily() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QMdnsEngine::Message ipv4;
        ipv4.setAddress(QHostAddress("192.168.1.77"));
        QMdnsEngine::Message ipv6;
        ipv6.setAddress(QHostAddress("fe80::1"));
        QMdnsEngine::Message unknown;

        adapter.sendMessage(ipv4);
        adapter.sendMessageToAll(ipv6);
        adapter.sendMessage(unknown);

        const QMap<QString, QString> ipv4Fields = {
            {QStringLiteral("scope"), QStringLiteral("unicast")},
            {QStringLiteral("address_family"), QStringLiteral("ipv4")}};
        const QMap<QString, QString> ipv6Fields = {
            {QStringLiteral("scope"), QStringLiteral("multicast_all")},
            {QStringLiteral("address_family"), QStringLiteral("ipv6")}};
        const QMap<QString, QString> unknownFields = {
            {QStringLiteral("scope"), QStringLiteral("unicast")},
            {QStringLiteral("address_family"), QStringLiteral("unknown")}};
        QCOMPARE(sink.events.at(0).fields, ipv4Fields);
        QCOMPARE(sink.events.at(1).fields, ipv6Fields);
        QCOMPARE(sink.events.at(2).fields, unknownFields);
        for (const DiagnosticEvent &event : sink.events)
            QVERIFY(event.name != "send_succeeded");
    }

    void delegateErrorIsSanitizedAndFlushedImmediately() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);
        QSignalSpy forwarded(&adapter, &QMdnsEngine::AbstractServer::error);

        delegate.fail("hostname=private\n192.168.1.77");

        const DiagnosticEvent event = findEvent(sink, "error");
        QCOMPARE(event.component, QString("discovery"));
        QCOMPARE(event.fields.value("reason"), QString("qmdns_engine"));
        QCOMPARE(event.fields.value("message"),
                 DiagnosticSanitizer::sanitizeText(QStringLiteral("hostname=private\n192.168.1.77")));
        QVERIFY(event.flushImmediately);
        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.at(0).at(0).toString(), QString("hostname=private\n192.168.1.77"));
    }

    void delegateErrorForwardsWhenSinkIsNull() {
        FakeAbstractServer delegate;
        MdnsServerAdapter adapter(&delegate, nullptr);
        QSignalSpy forwarded(&adapter, &QMdnsEngine::AbstractServer::error);

        delegate.fail(QStringLiteral("delegate error"));

        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.at(0).at(0).toString(), QStringLiteral("delegate error"));
    }
#else
    void sinkDefaultsCompileWithoutUxPlay() {
        CollectingSink sink;
        QCOMPARE(sink.events.size(), 0);
    }
#endif
};

QTEST_GUILESS_MAIN(MdnsServerAdapterTest)
#include "MdnsServerAdapterTest.moc"

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

    void delegateErrorIsSanitizedAndFlushedImmediately() {
        FakeAbstractServer delegate;
        CollectingSink sink;
        MdnsServerAdapter adapter(&delegate, &sink);

        delegate.fail("hostname=private\n192.168.1.77");

        const DiagnosticEvent event = findEvent(sink, "error");
        QCOMPARE(event.component, QString("discovery"));
        QCOMPARE(event.fields.value("reason"), QString("qmdns_engine"));
        QCOMPARE(event.fields.value("message"),
                 DiagnosticSanitizer::sanitizeText(QStringLiteral("hostname=private\n192.168.1.77")));
        QVERIFY(event.flushImmediately);
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

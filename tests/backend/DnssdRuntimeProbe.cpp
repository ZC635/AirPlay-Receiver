#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>

#include <QtTest/QtTest>
#include <QPointer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QRegularExpression>
#include <memory>

#include "backend/UxPlayDiscovery.h"
#include "platform/MdnsPublisher.h"
#include "lib/raop.h"
#include "support/CollectingDiagnosticLogSink.h"
#include <qmdnsengine/server.h>
#include <qmdnsengine/provider.h>
#include <qmdnsengine/hostname.h>
#include <qmdnsengine/dns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/query.h>
#include <qmdnsengine/record.h>

static bool inspectModules(const QString &phase) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool enumerated = Module32FirstW(snapshot, &module);
    if (!enumerated) { CloseHandle(snapshot); return false; }
    const QRegularExpression externalPattern(
        "^(?:(?:lib)?(?:dns[-_]?sd|dnssd|bonjour|mdnsresponder|mdnsnsp)(?:[-_.].*)?|(?:lib)?avahi(?:[-_].*)?)\\.dll$",
        QRegularExpression::CaseInsensitiveOption);
    bool external = false;
    do {
        QJsonObject entry{{"phase", phase}, {"path", QString::fromWCharArray(module.szExePath)}};
        QTextStream(stdout) << "DNSSD_MODULE " << QJsonDocument(entry).toJson(QJsonDocument::Compact) << Qt::endl;
        if (externalPattern.match(QString::fromWCharArray(module.szModule)).hasMatch()) {
            external = true;
            QTextStream(stdout) << "External DNS-SD module rejected: "
                                << QString::fromWCharArray(module.szExePath) << Qt::endl;
        }
    } while (Module32NextW(snapshot, &module));
    const bool complete = GetLastError() == ERROR_NO_MORE_FILES;
    CloseHandle(snapshot);
    return complete && !external;
}

class DnssdRuntimeProbe : public QObject {
    Q_OBJECT
private slots:
    void realDiscoveryPublishesAndReleasesResources() {
        QVERIFY(inspectModules("before-start"));
        raop_callbacks_t callbacks{};
        callbacks.audio_process = [](void *, raop_ntp_t *, audio_decode_struct *) {};
        callbacks.video_process = [](void *, raop_ntp_t *, video_decode_struct *) {};
        std::unique_ptr<raop_t, decltype(&raop_destroy)> raop(raop_init(&callbacks), raop_destroy);
        QVERIFY(raop);
        QCOMPARE(raop_init2(raop.get(), 0, "02:00:00:00:00:01", ""), 0);
        CollectingSink sink;
        UxPlayDiscoveryConfig config;
        const QByteArray identity = "DNS-SD Runtime Acceptance " + QByteArray::number(GetCurrentProcessId());
        config.receiverName = QString::fromUtf8(identity);
        config.diagnosticSink = &sink;
        UxPlayDiscovery discovery(config);
        QVERIFY2(discovery.start(raop.get(), 0), qPrintable(discovery.lastError()));
        QVERIFY(raop_is_running(raop.get()));
        QVERIFY(hasEvent(sink, "dns_sd_initialized"));
        QCOMPARE(countEvents(sink, "services_registered"), 1);
        const quint16 port = raop_get_port(raop.get());
        QVERIFY(port > 0);
        QCOMPARE(findEvent(sink, "http_server_started").fields.value("port"), QString::number(port));
        QTcpSocket http;
        http.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY2(http.waitForConnected(3000), qPrintable(http.errorString()));
        http.disconnectFromHost();

        QPointer<MdnsPublisher> publisher = discovery.findChild<MdnsPublisher *>();
        QVERIFY(publisher);
        QPointer<QMdnsEngine::Server> server = publisher->findChild<QMdnsEngine::Server *>();
        QVERIFY(server); // The production default factory must provide the real UDP server.
        QCOMPARE(publisher->findChildren<QMdnsEngine::Provider *>().size(), 2);
        QVERIFY(publisher->findChild<QMdnsEngine::Hostname *>());
        QList<QPointer<QObject>> resources;
        for (QObject *child : publisher->findChildren<QObject *>()) resources.append(child);
        QTRY_COMPARE_WITH_TIMEOUT(countEvents(sink, "hostname_registered"), 1, 10000);
        QCOMPARE(countEvents(sink, "error"), 0);

        // Observe actual multicast replies received by the production UDP server.
        // This uses host adapters but requires no remote device or external daemon.
        const QByteArray raopName = "020000000001@" + identity + "._raop._tcp.local.";
        const QByteArray airplayName = identity + "._airplay._tcp.local.";
        QMap<QByteArray, QMdnsEngine::Record> records;
        connect(server, &QMdnsEngine::AbstractServer::messageReceived, this,
                [&](const QMdnsEngine::Message &reply) {
            if (reply.isResponse()) {
                for (const auto &record : reply.records()) {
                    if (record.name() == raopName || record.name() == airplayName ||
                        (record.type() == QMdnsEngine::PTR && (record.target() == raopName || record.target() == airplayName)))
                        records.insert(record.name() + ':' + QByteArray::number(record.type()), record);
                }
            }
        });
        auto query = [&](QByteArray name, quint16 type) {
            QMdnsEngine::Message message;
            QMdnsEngine::Query question;
            question.setName(name);
            question.setType(type);
            message.addQuery(question);
            message.setAddress(QHostAddress("224.0.0.251"));
            message.setPort(5353);
            server->sendMessageToAll(message);
        };
        for (const auto &name : {QByteArray("_raop._tcp.local."), QByteArray("_airplay._tcp.local.")}) {
            query(name, QMdnsEngine::PTR);
            const QByteArray key = name + ":12";
            QTRY_VERIFY_WITH_TIMEOUT(records.contains(key), 5000);
            QCOMPARE(records.value(key).target(), name.startsWith("_raop") ? raopName : airplayName);
        }
        for (const auto &name : {raopName, airplayName}) {
            query(name, QMdnsEngine::SRV);
            query(name, QMdnsEngine::TXT);
            QTRY_VERIFY_WITH_TIMEOUT(records.contains(name + ":33") && records.contains(name + ":16"), 5000);
            QCOMPARE(records.value(name + ":33").port(), port);
            QVERIFY(!records.value(name + ":33").target().isEmpty());
            const auto txt = records.value(name + ":16").attributes();
            QVERIFY(!txt.isEmpty());
            if (name == raopName) {
                QCOMPARE(txt.value("txtvers"), QByteArray("1"));
                QVERIFY(!txt.value("am").isEmpty());
            } else {
                QCOMPARE(txt.value("deviceid"), QByteArray("02:00:00:00:00:01"));
                QVERIFY(!txt.value("features").isEmpty());
            }
        }
        QVERIFY(inspectModules("publishing"));
        discovery.stop();
        QVERIFY(!raop_is_running(raop.get()));
        QVERIFY(publisher.isNull());
        QVERIFY(server.isNull());
        for (const auto &resource : resources) QVERIFY(resource.isNull());
        QCOMPARE(countEvents(sink, "http_server_stopped"), 1);
        QCOMPARE(countEvents(sink, "services_unregistered"), 1);
        QCOMPARE(countEvents(sink, "dns_sd_destroyed"), 1);
        QTcpSocket stoppedHttp;
        stoppedHttp.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(!stoppedHttp.waitForConnected(1000));
        discovery.stop();
        QCOMPARE(countEvents(sink, "dns_sd_destroyed"), 1);
        QVERIFY(inspectModules("after-stop"));
    }
};

int main(int argc, char **argv) {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) return 90;
    QCoreApplication app(argc, argv);
    const QString fixture = qEnvironmentVariable("DNSSD_TEST_LOAD");
    HMODULE loadedFixture = nullptr;
    if (!fixture.isEmpty()) {
        loadedFixture = LoadLibraryExW(reinterpret_cast<LPCWSTR>(fixture.utf16()), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loadedFixture) return 91;
        auto fixtureFunction = reinterpret_cast<int (*)()>(GetProcAddress(loadedFixture, "dnssd_fixture"));
        if (!fixtureFunction || fixtureFunction() != 7) return 92;
        QTextStream(stdout) << "DNSSD_FIXTURE_LOADED " << fixture << Qt::endl;
    }
    DnssdRuntimeProbe test;
    const int result = QTest::qExec(&test, argc, argv);
    if (loadedFixture) FreeLibrary(loadedFixture);
    return result;
}
#include "DnssdRuntimeProbe.moc"

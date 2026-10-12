#include <winsock2.h>
#include <iphlpapi.h>
#include <QtTest/QtTest>
#include <QTcpSocket>
#include <array>
#include <atomic>
#include <memory>
extern "C" {
#include "lib/mirror_buffer.h"
#include "lib/raop_buffer.h"
#include "lib/httpd.h"
#include "lib/dnssd.h"
}

class UpstreamProtocolRegressionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); WSADATA data; QCOMPARE(WSAStartup(MAKEWORD(2, 2), &data), 0); }
    void cleanupTestCase() { WSACleanup(); }
    void dnssdLeavesMulticastSocketsToHost();
    void mirrorRekeyDiscardsPartialBlock();
    void noDataAudioPreservesSequence();
    void fragmentedClientDoesNotBlockOthers();
    void persistentConnectionRecognizesReverseResponse();
};

static int currentProcessMdnsSockets() {
    ULONG bytes = 0;
    if (GetExtendedUdpTable(nullptr, &bytes, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER) return -1;
    QByteArray storage(static_cast<qsizetype>(bytes), '\0');
    if (GetExtendedUdpTable(storage.data(), &bytes, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return -1;
    auto *table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID *>(storage.data());
    int count = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto &row = table->table[i];
        if (row.dwOwningPid == GetCurrentProcessId() && ntohs(static_cast<u_short>(row.dwLocalPort)) == 5353) ++count;
    }
    return count;
}

void UpstreamProtocolRegressionTest::dnssdLeavesMulticastSocketsToHost() {
    const int before = currentProcessMdnsSockets();
    QVERIFY(before >= 0);
    const char hardware[] = {2, 0, 0, 0, 0, 1};
    int error = -1;
    auto service = std::unique_ptr<dnssd_t, decltype(&dnssd_destroy)>(
        dnssd_init("External Publisher", 18, hardware, sizeof(hardware), 0, &error), dnssd_destroy);
    QVERIFY(service);
    QCOMPARE(error, 0);
    static char publicKey[] = "b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7";
    dnssd_set_pk(service.get(), publicKey);
    QCOMPARE(dnssd_register_raop(service.get(), 5000), 0);
    QCOMPARE(dnssd_register_airplay(service.get(), 5000), 0);
    int length = 0;
    QVERIFY(dnssd_get_airplay_txt(service.get(), &length));
    QVERIFY(length > 0);
    QCOMPARE(currentProcessMdnsSockets(), before);
    dnssd_unregister_airplay(service.get());
    dnssd_unregister_raop(service.get());
    service.reset();
    QCOMPARE(currentProcessMdnsSockets(), before);
}
void UpstreamProtocolRegressionTest::mirrorRekeyDiscardsPartialBlock() {
    auto log = std::unique_ptr<logger_t, decltype(&logger_destroy)>(logger_init(), logger_destroy);
    const std::array<unsigned char, 16> key{};
    const uint64_t oldId = 123, newId = 456;
    auto resumed = std::unique_ptr<mirror_buffer_t, decltype(&mirror_buffer_destroy)>(mirror_buffer_init(log.get(), key.data()), mirror_buffer_destroy);
    auto fresh = std::unique_ptr<mirror_buffer_t, decltype(&mirror_buffer_destroy)>(mirror_buffer_init(log.get(), key.data()), mirror_buffer_destroy);
    QVERIFY(resumed && fresh);
    mirror_buffer_init_aes(resumed.get(), &oldId);
    std::array<unsigned char, 17> partial{}, ignored{};
    mirror_buffer_decrypt(resumed.get(), partial.data(), ignored.data(), partial.size());
    mirror_buffer_init_aes(resumed.get(), &newId);
    mirror_buffer_init_aes(fresh.get(), &newId);
    std::array<unsigned char, 32> first{}, second{}, actual{}, expected{};
    mirror_buffer_decrypt(resumed.get(), first.data(), actual.data(), first.size());
    mirror_buffer_decrypt(fresh.get(), second.data(), expected.data(), second.size());
    QVERIFY2(actual == expected, "A re-keyed stream must decrypt identically to a fresh stream with the same key and connection ID");
}

void UpstreamProtocolRegressionTest::noDataAudioPreservesSequence() {
    auto log = std::unique_ptr<logger_t, decltype(&logger_destroy)>(logger_init(), logger_destroy);
    const std::array<unsigned char, 16> key{}, iv{};
    auto buffer = std::unique_ptr<raop_buffer_t, decltype(&raop_buffer_destroy)>(raop_buffer_init(log.get(), key.data(), iv.data()), raop_buffer_destroy);
    QVERIFY(buffer);
    for (unsigned short seq : {100, 101, 102}) {
        unsigned char packet[16] = {0x80, 0x60, 0, static_cast<unsigned char>(seq), 0, 0, 0, 1, 0, 0, 0, 0, 0, 0x68, 0x34, 0};
        QCOMPARE(raop_buffer_enqueue(buffer.get(), packet, sizeof(packet), 1), 1);
    }
    for (unsigned short expected : {100, 101, 102}) {
        unsigned int length = 0;
        uint32_t timestamp = 0;
        unsigned short sequence = 0;
        auto payload = std::unique_ptr<void, decltype(&free)>(raop_buffer_dequeue(buffer.get(), &length, &timestamp, &sequence, 0), free);
        QVERIFY(payload);
        QCOMPARE(sequence, expected);
        QCOMPARE(length, 4u);
        QCOMPARE(QByteArray(static_cast<char *>(payload.get()), length), QByteArray::fromHex("00683400"));
    }
}

struct TestHttpServer {
    logger_t *log = logger_init();
    httpd_t *server = nullptr;
    unsigned short port = 0;
    std::atomic<int> requests{0};
    std::atomic<int> reverseResponses{0};
    TestHttpServer() {
        logger_set_callback(log, [](void *, int, const char *) {}, nullptr);
        httpd_callbacks_t cb{};
        cb.opaque = this;
        cb.conn_init = [](void *opaque, unsigned char *, int, unsigned char *, int, unsigned int) -> void * { return opaque; };
        cb.conn_destroy = [](void *) {};
        cb.conn_request = [](void *opaque, http_request_t *request, http_response_t **response) {
            auto *self = static_cast<TestHttpServer *>(opaque);
            if (http_request_is_reverse(request)) {
                ++self->reverseResponses;
                return;
            }
            ++self->requests;
            *response = http_response_create();
            http_response_init(*response, "RTSP/1.0", 200, "OK");
            http_response_add_header(*response, "Content-Length", "0");
            http_response_finish(*response, nullptr, 0);
        };
        server = httpd_init(log, &cb, 1);
        if (server && httpd_start(server, &port) < 0) port = 0;
    }
    ~TestHttpServer() { if (server) { httpd_stop(server); httpd_destroy(server); } logger_destroy(log); }
};

void UpstreamProtocolRegressionTest::fragmentedClientDoesNotBlockOthers() {
    TestHttpServer server;
    QVERIFY(server.port);
    QTcpSocket partial, complete;
    partial.connectToHost(QHostAddress::LocalHost, server.port);
    QVERIFY(partial.waitForConnected(2000));
    partial.write("O", 1);
    QVERIFY(partial.waitForBytesWritten(2000));
    complete.connectToHost(QHostAddress::LocalHost, server.port);
    QVERIFY(complete.waitForConnected(2000));
    complete.write("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    QVERIFY(complete.waitForBytesWritten(2000));
    const bool responsive = complete.waitForReadyRead(2000);
    // Unblock a buggy baseline before assertions/destruction join its server thread.
    partial.abort();
    QVERIFY2(responsive, "A client sending fewer than eight bytes must not monopolize the HTTP server");
    QVERIFY(complete.readAll().contains("200 OK"));
}

void UpstreamProtocolRegressionTest::persistentConnectionRecognizesReverseResponse() {
    TestHttpServer server;
    QVERIFY(server.port);
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.port);
    QVERIFY(client.waitForConnected(2000));
    client.write("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    QVERIFY(client.waitForBytesWritten(2000));
    QVERIFY(client.waitForReadyRead(2000));
    QVERIFY(client.readAll().contains("200 OK"));
    client.write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    QVERIFY(client.waitForBytesWritten(2000));
    QVERIFY2(!client.waitForDisconnected(500), "A reverse HTTP response on an existing connection must not be parsed as an RTSP request");
    QCOMPARE(server.requests.load(), 1);
}

QTEST_GUILESS_MAIN(UpstreamProtocolRegressionTest)
#include "UpstreamProtocolRegressionTest.moc"

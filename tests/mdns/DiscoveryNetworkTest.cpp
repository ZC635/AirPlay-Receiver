#include "simulated_network.h"
#include "platform/MdnsPublisher.h"
#include "diagnostics/DiagnosticLogSink.h"
#include <qmdnsengine/dns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/query.h>
#include <qmdnsengine/record.h>
#include <qmdnsengine/server.h>
#include <QHostInfo>
#include <cstdio>
#include <functional>

class Sink final : public DiagnosticLogSink {
public:
    QList<DiagnosticEvent> events;
    int searches = 0;
    void record(DiagnosticEvent event) override {
        if (event.name == "search_received") ++searches;
        events.append(event);
    }
    bool saw(const QString &name, const QString &key, const QString &value) const {
        for (const auto &event : events)
            if (event.name == name && event.fields.value(key) == value) return true;
        return false;
    }
    int count(const QString &name) const {
        int n = 0;
        for (const auto &event : events) if (event.name == name) ++n;
        return n;
    }
};
static void require(bool value, const char *reason) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static bool waitFor(const std::function<bool()> &predicate, int timeout = 12000) {
    QElapsedTimer clock; clock.start();
    while (!predicate() && clock.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}
static void settle(int ms) {
    QElapsedTimer clock; clock.start();
    waitFor([&] { return clock.elapsed() >= ms; }, ms + 500);
}
static QList<QMdnsEngine::Record> recordsOn(int iface, bool v6 = false) {
    QList<QMdnsEngine::Record> records;
    for (const auto &packet : SimNetwork::sent) {
        if (packet.egress != iface || (packet.destination.protocol() == QAbstractSocket::IPv6Protocol) != v6) continue;
        QMdnsEngine::Message message;
        require(QMdnsEngine::fromPacket(packet.bytes, message), "outgoing DNS packet must decode");
        if (message.isResponse()) records.append(message.records());
    }
    return records;
}
static bool containsType(const QList<QMdnsEngine::Record> &records, quint16 type) {
    for (const auto &record : records) if (record.type() == type && record.ttl() > 0) return true;
    return false;
}
static bool servicesReady(int iface) {
    bool airplay = false, raop = false;
    for (const auto &record : recordsOn(iface)) {
        if (record.type() != QMdnsEngine::PTR || !record.ttl()) continue;
        airplay |= record.name() == "_airplay._tcp.local.";
        raop |= record.name() == "_raop._tcp.local.";
    }
    return airplay && raop;
}
// Services announced and the host address record announced: a client on the
// link can both browse and resolve. Re-announcements after a change are
// throttled per publisher, so tests wait for both before querying.
static bool linkReady(int iface) {
    return servicesReady(iface) && containsType(recordsOn(iface), QMdnsEngine::A);
}
// A response from another host, injected on a link, as a conflicting responder
// or a departing one would send it.
static void injectResponse(int iface, const QMdnsEngine::Record &record, const QHostAddress &source) {
    QMdnsEngine::Message message;
    message.setResponse(true);
    message.addRecord(record);
    QByteArray packet; QMdnsEngine::toPacket(message, packet);
    require(SimNetwork::inject(iface, packet, source, 5353), "injected response must reach joined socket");
}
static QMdnsEngine::Record addressRecord(const QByteArray &name, const QHostAddress &address, quint32 ttl) {
    QMdnsEngine::Record record;
    record.setName(name); record.setType(QMdnsEngine::A); record.setAddress(address); record.setTtl(ttl);
    return record;
}
static QMdnsEngine::Record srvRecord(const QByteArray &name, const QByteArray &target, quint32 ttl) {
    QMdnsEngine::Record record;
    record.setName(name); record.setType(QMdnsEngine::SRV); record.setTarget(target);
    record.setPort(7000); record.setTtl(ttl);
    return record;
}
static QByteArray ptrTarget(int iface, const QByteArray &type) {
    for (const auto &record : recordsOn(iface))
        if (record.type() == QMdnsEngine::PTR && record.ttl() && record.name() == type) return record.target();
    return {};
}
static void query(int iface, const QByteArray &name, quint16 type, quint16 port = 5353, bool v6 = false) {
    QMdnsEngine::Query question; question.setName(name); question.setType(type);
    QMdnsEngine::Message message; message.addQuery(question);
    if (port != 5353) message.setTransactionId(0x1234);
    QByteArray packet; QMdnsEngine::toPacket(message, packet);
    const QHostAddress phone(v6 ? "fe80::10" :
        (iface == SimNetwork::Lan || SimNetwork::overlap ? "172.16.177.10" : "172.17.153.10"));
    require(SimNetwork::inject(iface, packet, phone, port), "query must reach joined receiving socket");
}
static QByteArray discover(int iface, Sink &sink, quint16 port = 5353, bool v6 = false,
                           const QByteArray &serviceType = "_airplay._tcp.local.") {
    SimNetwork::sent.clear();
    const int before = sink.searches;
    query(iface, serviceType, QMdnsEngine::PTR, port, v6);
    require(sink.searches == before + 1, "application logs incoming service search");
    const auto records = recordsOn(iface, v6);
    require(containsType(records, QMdnsEngine::PTR) && containsType(records, QMdnsEngine::SRV)
            && containsType(records, QMdnsEngine::TXT), "client must receive PTR/SRV/TXT on ingress link");
    QByteArray hostname;
    for (const auto &record : records) if (record.type() == QMdnsEngine::SRV) {
        require(record.port() == 51537, "SRV receiver port must survive encoding"); hostname = record.target();
    }
    for (const auto &packet : SimNetwork::sent) {
        require(packet.egress == iface, "query response must not leak onto another link");
        require(packet.hopLimit == 255, "mDNS hop limit must be 255");
        require(packet.destination.isMulticast() == (port == 5353), "legacy source port preserves unicast reply");
        if (port != 5353) {
            QMdnsEngine::Message reply;
            require(QMdnsEngine::fromPacket(packet.bytes, reply), "legacy reply decodes");
            require(reply.transactionId() == 0x1234 && !reply.queries().isEmpty(), "legacy reply echoes ID and question");
            for (const auto &record : reply.records())
                require(!record.flushCache() && record.ttl() <= 10, "legacy reply must not flush multicast caches or retain long TTL");
        }
        if (v6) require(packet.destination.scopeId() == QString::number(iface), "IPv6 destination must carry scope");
    }
    SimNetwork::sent.clear();
    const auto type = v6 ? QMdnsEngine::AAAA : QMdnsEngine::A;
    query(iface, hostname, type, port, v6);
    const QHostAddress expected(v6 ? (iface == SimNetwork::Lan ? "fe80::12" : "fe80::5") :
        (iface == SimNetwork::Lan ? SimNetwork::lanAddress :
         (SimNetwork::overlap ? QString("172.16.176.253") : QString("172.17.153.166"))));
    bool resolved = false;
    for (const auto &record : recordsOn(iface, v6)) if (record.type() == type) {
        if (port != 5353)
            require(!record.flushCache() && record.ttl() <= 10, "legacy address reply must preserve conventional DNS cache behavior");
        require(record.address() == expected, "host records must belong to receiving interface"); resolved = true;
    }
    require(resolved, "client must resolve correct interface address");
    for (const auto &packet : SimNetwork::sent) {
        require(packet.egress == iface, "address reply must stay on ingress link");
        auto source = packet.source; source.setScopeId({});
        require(source == expected, "UDP source must belong to reply interface");
    }
    return hostname;
}
static bool sentHostnameProbe(int iface) {
    for (const auto &packet : SimNetwork::sent) {
        if (packet.egress != iface) continue;
        QMdnsEngine::Message message;
        require(QMdnsEngine::fromPacket(packet.bytes, message), "outgoing DNS packet must decode");
        if (message.isResponse()) continue;
        for (const auto &query : message.queries())
            if (query.type() == QMdnsEngine::A && query.name().endsWith(".local.")) return true;
    }
    return false;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    require(argc == 2, "provide a scenario name");
    const QString scenario = app.arguments().at(1);
    SimNetwork::campusUp = scenario != "lan_only" && scenario != "metadata_single";
    SimNetwork::defaultMulticastEgress = scenario == "dual_lan_default" || scenario == "campus_client"
        || !SimNetwork::campusUp ? SimNetwork::Lan : SimNetwork::Campus;
    SimNetwork::ipv6 = scenario == "ipv6";
    SimNetwork::overlap = scenario == "overlap";
    SimNetwork::ineligibleCampus = scenario == "ineligible";
    SimNetwork::missingMetadata = scenario.startsWith("metadata_");
    SimNetwork::loopback = scenario == "loopback";
    if (scenario == "join_retry") SimNetwork::failJoin = SimNetwork::Campus;
    Sink sink;
    MdnsPublisher publisher(&sink, {});
    const unsigned char txt[] = {9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1'};
    require(publisher.publish("Issue11-Lab", QByteArray::fromHex("020000000001"), 51537,
        reinterpret_cast<const char *>(txt), sizeof(txt), reinterpret_cast<const char *>(txt), sizeof(txt)), "publisher starts");
    // The hostname probes synchronously in its constructor; the server must
    // already have joined the links or the probe is lost and conflicts missed.
    require(sentHostnameProbe(SimNetwork::Lan), "startup hostname probe must reach the LAN");
    const QByteArray baseHostname = QHostInfo::localHostName().toUtf8().replace('.', '-');
    const QByteArray expectedHostname = baseHostname + ".local.";
    const QHostAddress foreign("172.16.177.99");
    if (scenario == "loopback") {
        // The probe window is open. Our own looped-back announcement (an A
        // record carrying one of our addresses) and a goodbye from a host that
        // is leaving are not claims on the name.
        injectResponse(SimNetwork::Lan, addressRecord(expectedHostname, QHostAddress(SimNetwork::lanAddress), 120),
                       QHostAddress(SimNetwork::lanAddress));
        injectResponse(SimNetwork::Lan, addressRecord(expectedHostname, QHostAddress("172.16.177.77"), 0), foreign);
    }
    if (scenario == "conflict") {
        // Another host already holds the name with a different address.
        injectResponse(SimNetwork::Lan, addressRecord(expectedHostname, QHostAddress("172.16.177.77"), 120), foreign);
    }
    if (scenario == "loopback" || scenario == "conflict") {
        // Once the hostname is registered the providers probe their names.
        require(waitFor([&] { return sink.count("hostname_registered") > 0; }), "hostname registers");
        const QByteArray serviceName = "Issue11-Lab._airplay._tcp.local.";
        const bool claim = scenario == "conflict";
        injectResponse(SimNetwork::Lan, srvRecord(serviceName, "other.local.", claim ? 120 : 0), foreign);
    }
    require(waitFor([] { return linkReady(SimNetwork::Lan); }),
        "LAN must receive announcement regardless of default route");
    if (scenario == "loopback") {
        require(ptrTarget(SimNetwork::Lan, "_airplay._tcp.local.") == "Issue11-Lab._airplay._tcp.local.",
                "a goodbye for the service name must not rename the service");
    }
    if (scenario == "conflict") {
        require(ptrTarget(SimNetwork::Lan, "_airplay._tcp.local.") == "Issue11-Lab-2._airplay._tcp.local.",
                "a live claim on the service name must rename the service");
    }
    const bool campusEligible = SimNetwork::campusUp && !SimNetwork::ineligibleCampus && !SimNetwork::failJoin;
    require(servicesReady(SimNetwork::Campus) == campusEligible,
        "announcements cover exactly the joined eligible links");
    if (scenario == "metadata_dual") {
        SimNetwork::sent.clear(); query(SimNetwork::Lan, "_airplay._tcp.local.", QMdnsEngine::PTR);
        require(SimNetwork::sent.isEmpty(), "ambiguous missing interface metadata must not guess an outlet");
        require(sink.saw("receive_dropped", "reason", "unknown_interface"), "missing metadata diagnosed");
    } else {
        discover(SimNetwork::Lan, sink, scenario == "legacy_unicast" ? 5354 : 5353);
        discover(SimNetwork::Lan, sink, 5353, false, "_raop._tcp.local.");
        if (campusEligible) discover(SimNetwork::Campus, sink);
    }
    if (scenario == "ipv6") {
        discover(SimNetwork::Lan, sink, 5353, true);
        discover(SimNetwork::Campus, sink, 5353, true);
        discover(SimNetwork::Campus, sink, 5354, true);
    }
    if (scenario == "hot_switch") {
        SimNetwork::campusUp = false; SimNetwork::sent.clear();
        require(waitFor([&] { return sink.saw("multicast_leave", "interface", "5"); }), "removed interface must leave group without queries");
        require(waitFor([] { return linkReady(SimNetwork::Lan); }), "remaining link must recover");
        discover(SimNetwork::Lan, sink);
        SimNetwork::campusUp = true; SimNetwork::sent.clear();
        require(waitFor([] { return linkReady(SimNetwork::Campus); }), "new interface must autonomously announce");
        discover(SimNetwork::Campus, sink); discover(SimNetwork::Lan, sink);
    }
    if (scenario == "address_change" || scenario == "stale_reply") {
        // An existing client has cached this address before the interface changes.
        const QHostAddress oldAddress("172.16.176.254");
        auto *server = publisher.findChild<QMdnsEngine::Server *>();
        QMdnsEngine::Message saved;
        auto connection = QObject::connect(server, &QMdnsEngine::AbstractServer::messageReceived,
            [&](const QMdnsEngine::Message &message) { saved.reply(message); });
        query(SimNetwork::Lan, "_airplay._tcp.local.", QMdnsEngine::PTR);
        QObject::disconnect(connection);
        SimNetwork::lanAddress = "172.16.176.252"; SimNetwork::sent.clear();
        require(waitFor([] { return servicesReady(SimNetwork::Lan); }), "address change must reannounce");
        bool withdrewOld = false, replacedCache = false;
        const auto hostRecordsUpdated = [&] {
            for (const auto &record : recordsOn(SimNetwork::Lan)) {
                if (record.type() != QMdnsEngine::A) continue;
                withdrewOld |= record.address() == oldAddress && record.ttl() == 0;
                replacedCache |= record.address() == QHostAddress("172.16.176.252")
                    && record.ttl() > 0 && record.flushCache();
            }
            return withdrewOld && replacedCache;
        };
        require(waitFor(hostRecordsUpdated), "address change must withdraw old A and proactively replace cached address");
        SimNetwork::sent.clear(); server->sendMessage(saved);
        require(SimNetwork::sent.isEmpty(), "reply from previous interface generation must not be sent");
        discover(SimNetwork::Lan, sink);
    }
    if (scenario == "join_retry") {
        require(sink.saw("multicast_join", "result", "failed"), "failed membership is diagnosed");
        SimNetwork::failJoin = 0; SimNetwork::sent.clear();
        require(waitFor([] { return servicesReady(SimNetwork::Campus); }), "membership retry must lead to announcement");
        require(waitFor([] { return containsType(recordsOn(SimNetwork::Campus), QMdnsEngine::A); }),
                "host address must be announced on a link once its join succeeds");
        require(!sink.saw("send_dropped", "reason", "inactive_interface"),
                "the hostname must not address a link the server has not joined");
        discover(SimNetwork::Campus, sink);
    }
    if (scenario == "loopback") {
        // Own multicast is looped back by the OS. Nothing we send may be
        // treated as a conflict by ourselves, yet a client on this host (a
        // browser tool, the acceptance probe) must still be answered.
        settle(300);
        const QByteArray hostname = discover(SimNetwork::Lan, sink);
        require(hostname == expectedHostname, "looped-back traffic must not rename the host");
        SimNetwork::sent.clear();
        const int searches = sink.searches;
        QMdnsEngine::Query question; question.setName("_airplay._tcp.local."); question.setType(QMdnsEngine::PTR);
        QMdnsEngine::Message browse; browse.addQuery(question);
        QByteArray packet; QMdnsEngine::toPacket(browse, packet);
        require(SimNetwork::inject(SimNetwork::Lan, packet, QHostAddress(SimNetwork::lanAddress), 5353),
                "own query reaches own socket");
        settle(100);
        require(sink.searches == searches + 1 && containsType(recordsOn(SimNetwork::Lan), QMdnsEngine::PTR),
                "a query from one of our own addresses must be answered like any other");
        discover(SimNetwork::Campus, sink);
    }
    if (scenario == "conflict") {
        require(discover(SimNetwork::Lan, sink) == baseHostname + "-2.local.",
                "a claim with a foreign address must rename the host");
    }
    if (scenario == "send_failure") {
        SimNetwork::failSend = SimNetwork::Lan; SimNetwork::sent.clear();
        query(SimNetwork::Lan, "_airplay._tcp.local.", QMdnsEngine::PTR);
        require(SimNetwork::sent.isEmpty(), "send failure must not reroute onto other link");
        require(sink.saw("send_result", "result", "failed"), "send failure diagnosed");
        discover(SimNetwork::Campus, sink);
        SimNetwork::failSend = 0; discover(SimNetwork::Lan, sink);
    }
    SimNetwork::sent.clear(); publisher.stop();
    bool goodbye = false;
    for (const auto &record : recordsOn(SimNetwork::Lan)) if (record.type() == QMdnsEngine::PTR && !record.ttl()) goodbye = true;
    require(goodbye, "stop must withdraw service on LAN");
    const auto count = SimNetwork::sent.size(); settle(2100);
    require(SimNetwork::sent.size() == count, "stopped publisher must not restart on pending timer");
    std::printf("PASS: %s; real_network_IO=none\n", scenario.toUtf8().constData());
}

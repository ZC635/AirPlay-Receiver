#include "simulated_network.h"
#include "platform/MdnsPublisher.h"
#include "diagnostics/DiagnosticLogSink.h"
#include <qmdnsengine/dns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/query.h>
#include <qmdnsengine/record.h>
#include <qmdnsengine/server.h>
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
};
static void require(bool value, const char *reason) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static bool waitFor(const std::function<bool()> &predicate, int timeout = 8500) {
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
static void query(int iface, const QByteArray &name, quint16 type, quint16 port = 5353, bool v6 = false) {
    QMdnsEngine::Query question; question.setName(name); question.setType(type);
    QMdnsEngine::Message message; message.addQuery(question);
    if (port != 5353) message.setTransactionId(0x1234);
    QByteArray packet; QMdnsEngine::toPacket(message, packet);
    const QHostAddress phone(v6 ? "fe80::10" :
        (iface == SimNetwork::Lan || SimNetwork::overlap ? "172.16.177.10" : "172.17.153.10"));
    require(SimNetwork::inject(iface, packet, phone, port), "query must reach joined receiving socket");
}
static void discover(int iface, Sink &sink, quint16 port = 5353, bool v6 = false,
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
    if (scenario == "join_retry") SimNetwork::failJoin = SimNetwork::Campus;
    Sink sink;
    MdnsPublisher publisher(&sink, {});
    const unsigned char txt[] = {9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1'};
    require(publisher.publish("Issue11-Lab", QByteArray::fromHex("020000000001"), 51537,
        reinterpret_cast<const char *>(txt), sizeof(txt), reinterpret_cast<const char *>(txt), sizeof(txt)), "publisher starts");
    require(waitFor([] { return servicesReady(SimNetwork::Lan); }),
        "LAN must receive announcement regardless of default route");
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
        require(waitFor([] { return servicesReady(SimNetwork::Lan); }), "remaining link must recover");
        discover(SimNetwork::Lan, sink);
        SimNetwork::campusUp = true; SimNetwork::sent.clear();
        require(waitFor([] { return servicesReady(SimNetwork::Campus); }), "new interface must autonomously announce");
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
        for (const auto &record : recordsOn(SimNetwork::Lan)) {
            if (record.type() != QMdnsEngine::A) continue;
            withdrewOld |= record.address() == oldAddress && record.ttl() == 0;
            replacedCache |= record.address() == QHostAddress("172.16.176.252")
                && record.ttl() > 0 && record.flushCache();
        }
        require(withdrewOld && replacedCache, "address change must withdraw old A and proactively replace cached address");
        SimNetwork::sent.clear(); server->sendMessage(saved);
        require(SimNetwork::sent.isEmpty(), "reply from previous interface generation must not be sent");
        discover(SimNetwork::Lan, sink);
    }
    if (scenario == "join_retry") {
        require(sink.saw("multicast_join", "result", "failed"), "failed membership is diagnosed");
        SimNetwork::failJoin = 0; SimNetwork::sent.clear();
        require(waitFor([] { return servicesReady(SimNetwork::Campus); }), "membership retry must lead to announcement");
        discover(SimNetwork::Campus, sink);
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

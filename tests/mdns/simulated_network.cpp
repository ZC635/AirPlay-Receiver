#include "simulated_network.h"
#include <cstring>
int SimNetworkInterface::flags() const {
    if (id == SimNetwork::Campus && SimNetwork::ineligibleCampus) return IsUp | CanMulticast;
    return IsUp | IsRunning | CanMulticast;
}
QList<QNetworkAddressEntry> SimNetworkInterface::addressEntries() const {
    QNetworkAddressEntry entry;
    entry.setIp(address);
    entry.setPrefixLength(prefix);
    QList<QNetworkAddressEntry> entries{entry};
    if (SimNetwork::ipv6) {
        entry.setIp(QHostAddress(id == SimNetwork::Lan ? "fe80::12" : "fe80::5"));
        entry.setPrefixLength(64);
        entries.append(entry);
    }
    return entries;
}
QList<SimNetworkInterface> SimNetworkInterface::allInterfaces() {
    QList<SimNetworkInterface> result{{SimNetwork::Lan, "LAN", QHostAddress(SimNetwork::lanAddress), 23}};
    if (SimNetwork::campusUp)
        result.append({SimNetwork::Campus, "CAMPUS",
            QHostAddress(SimNetwork::overlap ? "172.16.176.253" : "172.17.153.166"), SimNetwork::overlap ? 23 : 21});
    return result;
}
SimUdpSocket::SimUdpSocket() { SimNetwork::sockets.append(this); }
SimUdpSocket::~SimUdpSocket() { SimNetwork::sockets.removeOne(this); }
bool SimUdpSocket::bind(const QHostAddress &address, quint16, QAbstractSocket::BindMode) {
    family = address.protocol(); bound = true; return true;
}
QAbstractSocket::SocketState SimUdpSocket::state() const {
    return bound ? QAbstractSocket::BoundState : QAbstractSocket::UnconnectedState;
}
bool SimUdpSocket::joinMulticastGroup(const QHostAddress &group, const SimNetworkInterface &iface) {
    if (iface.id == SimNetwork::failJoin || (group.protocol() == QAbstractSocket::IPv6Protocol && !SimNetwork::ipv6)) return false;
    members.insert(iface.id); return true;
}
bool SimUdpSocket::leaveMulticastGroup(const QHostAddress &, const SimNetworkInterface &iface) {
    return members.remove(iface.id);
}
void SimUdpSocket::setMulticastInterface(const SimNetworkInterface &iface) {
    explicitInterface = iface.id; ++SimNetwork::selectInterfaceCalls;
}
qint64 SimUdpSocket::writeDatagram(const QByteArray &bytes, const QHostAddress &dest, quint16 port) {
    QNetworkDatagram datagram(bytes, dest, port);
    return writeDatagram(datagram);
}
qint64 SimUdpSocket::writeDatagram(const QNetworkDatagram &datagram) {
    const auto dest = datagram.destinationAddress();
#ifdef Q_OS_WIN
    // Reproduced with native QUdpSocket: IPv4 per-packet TTL fails with WSAEINVAL.
    if (dest.protocol() == QAbstractSocket::IPv4Protocol && datagram.hopLimit() >= 0) return -1;
#endif
    if (dest.protocol() == QAbstractSocket::IPv6Protocol && !SimNetwork::ipv6) return -1;
    int egress = datagram.interfaceIndex();
    if (!egress && dest.isMulticast()) egress = explicitInterface ? explicitInterface : SimNetwork::defaultMulticastEgress;
    if (!egress) {
        for (const auto &iface : SimNetworkInterface::allInterfaces())
            if (dest.isInSubnet(iface.address, iface.prefix)) egress = iface.id;
    }
    if (!egress || egress == SimNetwork::failSend || (egress == SimNetwork::Campus && !SimNetwork::campusUp)) return -1;
    const int hop = datagram.hopLimit() >= 0 ? datagram.hopLimit()
        : (dest.isMulticast() ? multicastHopLimit : unicastHopLimit);
    SimNetwork::sent.append({datagram.data(), dest, quint16(datagram.destinationPort()), egress,
                            datagram.senderAddress(), hop});
    if (SimNetwork::loopback && dest.isMulticast()) {
        // Asynchronous like the kernel: the sender's own readyRead fires later.
        const auto bytes = datagram.data();
        const auto source = datagram.senderAddress();
        for (auto *socket : SimNetwork::sockets) {
            if (socket->family != dest.protocol() || !socket->members.contains(egress)) continue;
            QPointer<SimUdpSocket> target(socket);
            QTimer::singleShot(0, socket, [target, bytes, source, egress] {
                if (target) target->inject(bytes, source, 5353, egress);
            });
        }
    }
    return datagram.data().size();
}
qint64 SimUdpSocket::pendingDatagramSize() const { return incoming.isEmpty() ? -1 : incoming.head().data().size(); }
QNetworkDatagram SimUdpSocket::receiveDatagram(qint64) {
    if (incoming.isEmpty()) return {};
    ++SimNetwork::receiveCount; return incoming.dequeue();
}
qint64 SimUdpSocket::readDatagram(char *data, qint64 size, QHostAddress *address, quint16 *port) {
    const auto packet = receiveDatagram();
    const auto n = qMin(size, qint64(packet.data().size()));
    std::memcpy(data, packet.data().constData(), size_t(n));
    *address = packet.senderAddress(); *port = packet.senderPort(); return n;
}
void SimUdpSocket::inject(const QByteArray &bytes, const QHostAddress &address, quint16 port, int index) {
    QNetworkDatagram datagram(bytes, address.protocol() == QAbstractSocket::IPv4Protocol
        ? QHostAddress("224.0.0.251") : QHostAddress("ff02::fb"), 5353);
    datagram.setSender(address, port);
    datagram.setInterfaceIndex(SimNetwork::missingMetadata ? 0 : index);
    incoming.enqueue(datagram); emit readyRead();
}
bool SimNetwork::inject(int ingress, const QByteArray &bytes, const QHostAddress &source, quint16 port) {
    for (auto *socket : sockets) {
        if (socket->family == source.protocol() && socket->members.contains(ingress)) {
            socket->inject(bytes, source, port, ingress); return true;
        }
    }
    return false;
}

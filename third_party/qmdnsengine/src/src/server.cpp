/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Nathan Osman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#if defined(_WIN32) && !defined(SIMULATE_QMDNS_NETWORK)
#define QMDNSENGINE_IP_NOTIFICATIONS 1
#include <iphlpapi.h>
#include <netioapi.h>
#endif
#include <QNetworkDatagram>
#include <QStringList>
#include <iterator>
#include <QVariant>
#include <qmdnsengine/dns.h>
#include <qmdnsengine/mdns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/server.h>
#include "server_p.h"
#include "networkinterface_p.h"

using namespace QMdnsEngine;

namespace {
QString membershipKey(uint index, const QHostAddress &address) {
    return QString::number(index) + (address.protocol() == QAbstractSocket::IPv4Protocol ? ":4" : ":6");
}
QString familyName(const QHostAddress &address) {
    return address.protocol() == QAbstractSocket::IPv4Protocol ? "ipv4" : "ipv6";
}
// True when the datagram came from one of this host's own addresses on the
// receiving link: our multicast is looped back to our own socket by default.
// Such datagrams are still delivered (a client on this host must be able to
// discover us); the hostname and prober use the flag to ignore their own echo.
bool ownAddress(QHostAddress sender, const QNetworkInterface &iface) {
    sender.setScopeId({});
    for (const auto &entry : iface.addressEntries()) {
        auto local = entry.ip();
        local.setScopeId({});
        if (local == sender) return true;
    }
    return false;
}
#ifdef QMDNSENGINE_IP_NOTIFICATIONS
// Called by the system on a worker thread; only hop back to the Qt thread.
void WINAPI onInterfaceChange(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
    QMetaObject::invokeMethod(static_cast<ServerPrivate *>(context), &ServerPrivate::scheduleRefresh, Qt::QueuedConnection);
}
void WINAPI onAddressChange(PVOID context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
    QMetaObject::invokeMethod(static_cast<ServerPrivate *>(context), &ServerPrivate::scheduleRefresh, Qt::QueuedConnection);
}
#endif
}

ServerPrivate::ServerPrivate(Server *server) : QObject(server), q(server)
{
    connect(&timer, &QTimer::timeout, this, &ServerPrivate::onTimeout);
    connect(&changeTimer, &QTimer::timeout, this, &ServerPrivate::onTimeout);
    connect(&ipv4Socket, &QUdpSocket::readyRead, this, &ServerPrivate::onReadyRead);
    connect(&ipv6Socket, &QUdpSocket::readyRead, this, &ServerPrivate::onReadyRead);
    // Bursts of change notifications (one per address) collapse into one refresh.
    changeTimer.setSingleShot(true);
    changeTimer.setInterval(250);
#ifdef QMDNSENGINE_IP_NOTIFICATIONS
    if (NotifyIpInterfaceChange(AF_UNSPEC, &onInterfaceChange, this, FALSE, &interfaceNotification) != NO_ERROR)
        interfaceNotification = nullptr;
    if (NotifyUnicastIpAddressChange(AF_UNSPEC, &onAddressChange, this, FALSE, &addressNotification) != NO_ERROR)
        addressNotification = nullptr;
#endif
    // With change notifications the poll is only a safety net; without them it
    // is the only way to observe hotplug and address changes.
    const bool notified = interfaceNotification && addressNotification;
    timer.setInterval(notified ? 30000 : 2000);
    timer.start();
    // Membership is established lazily (first send or first poll) so that the
    // publisher can attach its diagnostic observers before the initial joins
    // and the hostname's first probe still reaches every link.
    QTimer::singleShot(0, this, &ServerPrivate::onTimeout);
}

ServerPrivate::~ServerPrivate()
{
#ifdef QMDNSENGINE_IP_NOTIFICATIONS
    // Blocks until in-flight callbacks return; queued invocations they posted
    // are discarded with this object.
    if (interfaceNotification) CancelMibChangeNotify2(interfaceNotification);
    if (addressNotification) CancelMibChangeNotify2(addressNotification);
#endif
}

void ServerPrivate::ensureInitialized()
{
    if (!initialized) refreshInterfaces();
}

void ServerPrivate::scheduleRefresh()
{
    changeTimer.start();
}

bool ServerPrivate::bindSocket(QUdpSocket &socket, const QHostAddress &address)
{
    if (socket.state() == QAbstractSocket::BoundState) return true;
    if (socket.bind(address, MdnsPort, QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        socket.setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
#ifdef Q_OS_WIN
        // Winsock rejects IPv4 IP_TTL ancillary data (WSAEINVAL). Set the
        // socket's unicast TTL instead; multicast uses MulticastTtlOption.
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            const int ttl = 255;
            if (::setsockopt(static_cast<SOCKET>(socket.socketDescriptor()), IPPROTO_IP, IP_TTL,
                             reinterpret_cast<const char *>(&ttl), sizeof(ttl)) != 0) {
                socket.close();
                if (!failedBinds.contains(&socket)) {
                    emit q->transportEvent("socket_configure", {{"address_family", "ipv4"},
                        {"reason", "hop_limit"}, {"result", "failed"}});
                    emit q->error("Unable to configure IPv4 TTL");
                }
                failedBinds.insert(&socket);
                return false;
            }
        }
#endif
        failedBinds.remove(&socket);
        return true;
    }
    // A socket that cannot bind is retried every poll; report it once.
    if (!failedBinds.contains(&socket)) {
        emit q->transportEvent("socket_bind", {{"address_family", familyName(address)}, {"result", "failed"},
            {"message", socket.errorString()}});
        emit q->error(socket.errorString());
    }
    failedBinds.insert(&socket);
    return false;
}

void ServerPrivate::refreshInterfaces()
{
    QMap<QString, Membership> desired;
    QMap<QString, QString> current;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!usableInterface(iface)) continue;
        for (const auto &group : {MdnsIpv4Address, MdnsIpv6Address}) {
            QStringList addresses;
            for (const auto &entry : iface.addressEntries()) {
                if (entry.ip().protocol() == group.protocol() && usableAddress(entry.ip()))
                    addresses.append(entry.ip().toString() + "/" + QString::number(entry.prefixLength()));
            }
            if (addresses.isEmpty()) continue;
            addresses.sort();
            const auto key = membershipKey(iface.index(), group);
            const auto signature = addresses.join(';');
            desired.insert(key, {iface, group, signature});
            current.insert(key, signature);
        }
    }
    bool changed = current != snapshot;
    for (auto it = members.begin(); it != members.end();) {
        if (!desired.contains(it.key()) || desired.value(it.key()).addresses != it->addresses) {
            auto &socket = it->group.protocol() == QAbstractSocket::IPv4Protocol ? ipv4Socket : ipv6Socket;
            const bool left = socket.leaveMulticastGroup(it->group, it->iface);
            emit q->transportEvent("multicast_leave", {{"interface", QString::number(it->iface.index())},
                {"address_family", familyName(it->group)}, {"result", left ? "accepted" : "failed"}});
            it = members.erase(it);
        } else ++it;
    }
    for (auto it = failedJoins.begin(); it != failedJoins.end();)
        it = desired.contains(*it) ? std::next(it) : failedJoins.erase(it);
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        if (members.contains(it.key())) continue;
        auto &socket = it->group.protocol() == QAbstractSocket::IPv4Protocol ? ipv4Socket : ipv6Socket;
        const auto any = it->group.protocol() == QAbstractSocket::IPv4Protocol ? QHostAddress::AnyIPv4 : QHostAddress::AnyIPv6;
        const bool joined = bindSocket(socket, any) && socket.joinMulticastGroup(it->group, it->iface);
        if (joined) {
            Membership member = it.value();
            member.generation = ++generation;
            members.insert(it.key(), member);
            failedJoins.remove(it.key());
            changed = true;
            emit q->transportEvent("multicast_join", {{"interface", QString::number(it->iface.index())},
                {"address_family", familyName(it->group)}, {"result", "accepted"}});
        } else if (!failedJoins.contains(it.key())) {
            // One link that cannot be joined (virtual adapters, VPN tunnels) is
            // not an error for the server: it is diagnosed once and retried.
            failedJoins.insert(it.key());
            emit q->transportEvent("multicast_join", {{"interface", QString::number(it->iface.index())},
                {"address_family", familyName(it->group)}, {"result", "failed"},
                {"message", socket.errorString()}});
        }
    }
    snapshot = current;
    const bool notify = initialized && changed;
    initialized = true;
    if (notify) emit q->interfacesChanged();
}

void ServerPrivate::onTimeout() { refreshInterfaces(); }

void ServerPrivate::onReadyRead()
{
    auto *socket = qobject_cast<QUdpSocket *>(sender());
    if (!socket) return;
    while (socket->hasPendingDatagrams()) {
        const auto datagram = socket->receiveDatagram();
        if (!datagram.isValid()) {
            emit q->error(socket->errorString());
            break;
        }
        uint index = datagram.interfaceIndex();
        if (!index) {
            // Metadata may be unavailable. Only a single eligible link is unambiguous.
            QList<uint> candidates;
            for (const auto &member : members)
                if (member.group.protocol() == datagram.senderAddress().protocol())
                    candidates.append(member.iface.index());
            if (candidates.size() == 1) index = candidates.first();
        }
        const auto key = membershipKey(index, datagram.senderAddress());
        if (!index || !members.contains(key)) {
            // A link that came up since the last poll, or one we never joined:
            // reconcile through the debounced path so a stream of such packets
            // cannot enumerate the interfaces on every batch.
            if (index) scheduleRefresh();
            emit q->transportEvent("receive_dropped", {{"reason", "unknown_interface"}});
            continue;
        }
        Message message;
        if (!fromPacket(datagram.data(), message)) continue;
        message.setAddress(datagram.senderAddress());
        message.setPort(datagram.senderPort());
        message.setInterfaceIndex(index);
        message.setInterfaceGeneration(members.value(key).generation);
        message.setSenderIsLocal(ownAddress(datagram.senderAddress(), members.value(key).iface));
        emit q->messageReceived(message);
    }
}

uint ServerPrivate::routeInterface(const QHostAddress &destination) const
{
    // No interface was given (a message composed by the caller rather than
    // by reply()): pick the joined link whose subnet contains the destination,
    // as the routing table would.
    for (const auto &member : members) {
        if (member.group.protocol() != destination.protocol()) continue;
        for (const auto &entry : member.iface.addressEntries()) {
            if (entry.ip().protocol() == destination.protocol()
                && destination.isInSubnet(entry.ip(), entry.prefixLength()))
                return member.iface.index();
        }
    }
    return 0;
}

void ServerPrivate::send(const QByteArray &packet, const QHostAddress &destination, quint16 port, uint index)
{
    if (!index) {
        if (destination.isMulticast()) {
            // Multicast without a link means every joined link of that family.
            const auto current = members;
            for (const auto &member : current)
                if (member.group.protocol() == destination.protocol())
                    send(packet, destination, port, member.iface.index());
            return;
        }
        index = routeInterface(destination);
        if (!index) {
            // Nothing we joined reaches it directly; let the OS route it.
            auto &socket = destination.protocol() == QAbstractSocket::IPv4Protocol ? ipv4Socket : ipv6Socket;
            const qint64 written = socket.state() == QAbstractSocket::BoundState
                ? socket.writeDatagram(packet, destination, port) : -1;
            QMap<QString, QString> fields{{"interface", "0"}, {"address_family", familyName(destination)},
                {"scope", "unicast"}, {"bytes", QString::number(written)},
                {"result", written == packet.size() ? "accepted" : "failed"}};
            if (written != packet.size()) fields.insert("message", socket.errorString());
            emit q->transportEvent("send_result", fields);
            return;
        }
    }
    const auto key = membershipKey(index, destination);
    if (!members.contains(key)) {
        emit q->transportEvent("send_dropped", {{"interface", QString::number(index)}, {"reason", "inactive_interface"}});
        return;
    }
    const auto &member = members.value(key);
    auto &socket = destination.protocol() == QAbstractSocket::IPv4Protocol ? ipv4Socket : ipv6Socket;
    QHostAddress local;
    for (const auto &entry : member.iface.addressEntries()) {
        const auto address = entry.ip();
        if (address.protocol() != destination.protocol() || !usableAddress(address)) continue;
        if (local.isNull()) local = address;
        if (destination.isInSubnet(address, entry.prefixLength()) ||
            (destination.protocol() == QAbstractSocket::IPv6Protocol &&
             (destination.isLinkLocal() || destination.isMulticast()) && address.isLinkLocal())) {
            local = address;
            break;
        }
    }
    auto target = destination;
    if (target.protocol() == QAbstractSocket::IPv6Protocol && (target.isMulticast() || target.isLinkLocal()))
        target.setScopeId(QString::number(index));
    QNetworkDatagram datagram(packet, target, port);
    datagram.setInterfaceIndex(index);
    datagram.setSender(local, MdnsPort);
#ifdef Q_OS_WIN
    if (target.protocol() == QAbstractSocket::IPv6Protocol) datagram.setHopLimit(255);
#else
    datagram.setHopLimit(255);
#endif
    // Explicit multicast selection also covers Qt backends that omit ancillary interface data.
    if (target.isMulticast()) socket.setMulticastInterface(member.iface);
    const qint64 written = socket.writeDatagram(datagram);
    // A failed write is reported through the transport event; a link that went
    // away between polls must not surface as a server error.
    QMap<QString, QString> fields{{"interface", QString::number(index)},
        {"address_family", familyName(target)}, {"scope", target.isMulticast() ? "multicast" : "unicast"},
        {"bytes", QString::number(written)}, {"result", written == packet.size() ? "accepted" : "failed"}};
    if (written != packet.size()) fields.insert("message", socket.errorString());
    emit q->transportEvent("send_result", fields);
}

Server::Server(QObject *parent) : AbstractServer(parent), d(new ServerPrivate(this)) {}

void Server::sendMessage(const Message &message)
{
    d->ensureInitialized();
    const auto key = membershipKey(message.interfaceIndex(), message.address());
    if (message.interfaceGeneration() && d->members.contains(key)
        && d->members.value(key).generation != message.interfaceGeneration()) {
        // The receiving link was re-joined with different addresses after
        // this reply was composed; other links are unaffected.
        emit transportEvent("send_dropped", {{"interface", QString::number(message.interfaceIndex())},
            {"reason", "stale_interface"}});
        return;
    }
    QByteArray packet;
    toPacket(message, packet);
    d->send(packet, message.address(), message.port(), message.interfaceIndex());
}

bool Server::isJoined(uint interfaceIndex, const QHostAddress &group) const
{
    return d->members.contains(membershipKey(interfaceIndex, group));
}

void Server::sendMessageToAll(const Message &message)
{
    d->ensureInitialized();
    QByteArray packet;
    toPacket(message, packet);
    const auto members = d->members;
    for (const auto &member : members)
        d->send(packet, member.group, MdnsPort, member.iface.index());
}

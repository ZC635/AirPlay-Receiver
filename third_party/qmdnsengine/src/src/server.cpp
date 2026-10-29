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
#include <QNetworkDatagram>
#include <QStringList>
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
}

ServerPrivate::ServerPrivate(Server *server) : QObject(server), q(server)
{
    connect(&timer, &QTimer::timeout, this, &ServerPrivate::onTimeout);
    connect(&ipv4Socket, &QUdpSocket::readyRead, this, &ServerPrivate::onReadyRead);
    connect(&ipv6Socket, &QUdpSocket::readyRead, this, &ServerPrivate::onReadyRead);
    timer.setInterval(2000);
    timer.start();
    // Let the publisher attach its diagnostic observers before initialization.
    QTimer::singleShot(0, this, &ServerPrivate::onTimeout);
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
                emit q->transportEvent("socket_configure", {{"address_family", "ipv4"},
                    {"reason", "hop_limit"}, {"result", "failed"}});
                emit q->error("Unable to configure IPv4 TTL");
                return false;
            }
        }
#endif
        return true;
    }
    emit q->transportEvent("socket_bind", {{"address_family", familyName(address)}, {"result", "failed"}});
    emit q->error(socket.errorString());
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
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        if (members.contains(it.key())) continue;
        auto &socket = it->group.protocol() == QAbstractSocket::IPv4Protocol ? ipv4Socket : ipv6Socket;
        const auto any = it->group.protocol() == QAbstractSocket::IPv4Protocol ? QHostAddress::AnyIPv4 : QHostAddress::AnyIPv6;
        const bool joined = bindSocket(socket, any) && socket.joinMulticastGroup(it->group, it->iface);
        emit q->transportEvent("multicast_join", {{"interface", QString::number(it->iface.index())},
            {"address_family", familyName(it->group)}, {"result", joined ? "accepted" : "failed"}});
        if (joined) {
            members.insert(it.key(), it.value());
            changed = true;
        } else emit q->error(socket.errorString());
    }
    snapshot = current;
    const bool notify = initialized && changed;
    initialized = true;
    if (notify) {
        ++generation;
        emit q->interfacesChanged();
    }
}

void ServerPrivate::onTimeout() { refreshInterfaces(); }

void ServerPrivate::onReadyRead()
{
    auto *socket = qobject_cast<QUdpSocket *>(sender());
    if (!socket) return;
    refreshInterfaces();
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
        if (!index || !members.contains(membershipKey(index, datagram.senderAddress()))) {
            emit q->transportEvent("receive_dropped", {{"reason", "unknown_interface"}});
            continue;
        }
        Message message;
        if (!fromPacket(datagram.data(), message)) continue;
        message.setAddress(datagram.senderAddress());
        message.setPort(datagram.senderPort());
        message.setInterfaceIndex(index);
        message.setInterfaceGeneration(generation);
        emit q->messageReceived(message);
    }
}

void ServerPrivate::send(const QByteArray &packet, const QHostAddress &destination, quint16 port, uint index)
{
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
    emit q->transportEvent("send_result", {{"interface", QString::number(index)},
        {"address_family", familyName(target)}, {"scope", target.isMulticast() ? "multicast" : "unicast"},
        {"bytes", QString::number(written)}, {"result", written == packet.size() ? "accepted" : "failed"}});
    if (written != packet.size()) emit q->error(socket.errorString());
}

Server::Server(QObject *parent) : AbstractServer(parent), d(new ServerPrivate(this)) {}

void Server::sendMessage(const Message &message)
{
    d->refreshInterfaces();
    if (message.interfaceGeneration() && message.interfaceGeneration() != d->generation) {
        emit transportEvent("send_dropped", {{"reason", "stale_interface"}});
        return;
    }
    QByteArray packet;
    toPacket(message, packet);
    d->send(packet, message.address(), message.port(), message.interfaceIndex());
}

void Server::sendMessageToAll(const Message &message)
{
    d->refreshInterfaces();
    QByteArray packet;
    toPacket(message, packet);
    const auto members = d->members;
    for (const auto &member : members)
        d->send(packet, member.group, MdnsPort, member.iface.index());
}

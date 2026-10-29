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

#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>

#include <qmdnsengine/abstractserver.h>
#include <qmdnsengine/dns.h>
#include <qmdnsengine/hostname.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/mdns.h>
#include <qmdnsengine/query.h>
#include <qmdnsengine/record.h>

#include "hostname_p.h"
#include "networkinterface_p.h"

using namespace QMdnsEngine;

HostnamePrivate::HostnamePrivate(Hostname *hostname, AbstractServer *server)
    : QObject(hostname),
      server(server),
      q(hostname)
{
    connect(server, &AbstractServer::messageReceived, this, &HostnamePrivate::onMessageReceived);
    connect(&registrationTimer, &QTimer::timeout, this, &HostnamePrivate::onRegistrationTimeout);
    connect(&rebroadcastTimer, &QTimer::timeout, this, &HostnamePrivate::onRebroadcastTimeout);

    registrationTimer.setInterval(2 * 1000);
    registrationTimer.setSingleShot(true);

    rebroadcastTimer.setInterval(30 * 60 * 1000);
    rebroadcastTimer.setSingleShot(true);

    // Immediately assert the hostname
    onRebroadcastTimeout();
}

void HostnamePrivate::assertHostname()
{
    // Begin with the local hostname and replace any "." with "-" (I'm looking
    // at you, macOS)
    QByteArray localHostname = QHostInfo::localHostName().toUtf8();
    localHostname = localHostname.replace('.', '-');

    // If the suffix > 1, then append a "-2", "-3", etc. to the hostname to
    // aid in finding one that is unique and not in use
    hostname = (hostnameSuffix == 1 ? localHostname:
        localHostname + "-" + QByteArray::number(hostnameSuffix)) + ".local.";

    // Compose a query for A and AAAA records matching the hostname
    Query ipv4Query;
    ipv4Query.setName(hostname);
    ipv4Query.setType(A);
    Query ipv6Query;
    ipv6Query.setName(hostname);
    ipv6Query.setType(AAAA);
    Message message;
    message.addQuery(ipv4Query);
    message.addQuery(ipv6Query);

    server->sendMessageToAll(message);

    // If no reply is received after two seconds, the hostname is available
    registrationTimer.start();
}

QList<Record> HostnamePrivate::generateRecords(uint interfaceIndex, quint16 type)
{
    QList<Record> records;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (uint(iface.index()) != interfaceIndex || !usableInterface(iface)) continue;
        for (const auto &entry : iface.addressEntries()) {
            auto address = entry.ip();
            if (!usableAddress(address)) continue;
            if ((type == A && address.protocol() == QAbstractSocket::IPv4Protocol) ||
                (type == AAAA && address.protocol() == QAbstractSocket::IPv6Protocol)) {
                Record record;
                record.setName(hostname);
                record.setType(type);
                record.setAddress(address);
                record.setFlushCache(true);
                record.setTtl(120);
                records.append(record);
            }
        }
    }
    return records;
}

void HostnamePrivate::onMessageReceived(const Message &message)
{
    if (message.isResponse()) {
        if (hostnameRegistered) {
            return;
        }
        const auto records = message.records();
        for (const Record &record : records) {
            if (record.ttl() > 0 && (record.type() == A || record.type() == AAAA) && record.name() == hostname) {
                ++hostnameSuffix;
                assertHostname();
            }
        }
    } else {
        if (!hostnameRegistered) {
            return;
        }
        Message reply;
        reply.reply(message);
        const auto queries = message.queries();
        for (const Query &query : queries) {
            if ((query.type() == A || query.type() == AAAA) && query.name() == hostname) {
                for (const auto &record : generateRecords(message.interfaceIndex(), query.type()))
                    reply.addRecord(record);
            }
        }
        if (reply.records().count()) {
            server->sendMessage(reply);
        }
    }
}

void HostnamePrivate::onRegistrationTimeout()
{
    hostnameRegistered = true;
    sendAddressRecords(true);
    announcedRecords.clear();
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!usableInterface(iface)) continue;
        auto records = generateRecords(iface.index(), A);
        records.append(generateRecords(iface.index(), AAAA));
        if (!records.isEmpty()) announcedRecords.insert(iface.index(), records);
    }
    sendAddressRecords(false);
    if (hostname != hostnamePrev) {
        emit q->hostnameChanged(hostname);
    }

    // Re-assert the hostname in half an hour
    rebroadcastTimer.start();
}

HostnamePrivate::~HostnamePrivate() { sendAddressRecords(true); }

void HostnamePrivate::sendAddressRecords(bool goodbye)
{
    for (auto it = announcedRecords.cbegin(); it != announcedRecords.cend(); ++it) {
        bool ipv4 = false, ipv6 = false;
        for (const auto &record : it.value()) {
            ipv4 |= record.type() == A;
            ipv6 |= record.type() == AAAA;
        }
        for (const auto &group : {MdnsIpv4Address, MdnsIpv6Address}) {
            if (group.protocol() == QAbstractSocket::IPv4Protocol ? !ipv4 : !ipv6) continue;
            Message message;
            message.setResponse(true);
            message.setInterfaceIndex(it.key());
            message.setAddress(group);
            message.setPort(MdnsPort);
            for (auto record : it.value()) {
                if (goodbye) record.setTtl(0);
                message.addRecord(record);
            }
            server->sendMessage(message);
        }
    }
}

void HostnamePrivate::onRebroadcastTimeout()
{
    hostnamePrev = hostname;
    hostnameRegistered = false;
    hostnameSuffix = 1;

    assertHostname();
}

Hostname::Hostname(AbstractServer *server, QObject *parent)
    : QObject(parent),
      d(new HostnamePrivate(this, server))
{
}

bool Hostname::isRegistered() const
{
    return d->hostnameRegistered;
}

QByteArray Hostname::hostname() const
{
    return d->hostname;
}

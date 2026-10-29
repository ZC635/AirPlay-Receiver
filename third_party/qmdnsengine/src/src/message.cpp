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

#include <qmdnsengine/mdns.h>
#include <qmdnsengine/message.h>
#include <qmdnsengine/query.h>
#include <qmdnsengine/record.h>

#include "message_p.h"

using namespace QMdnsEngine;

MessagePrivate::MessagePrivate()
    : port(0),
      transactionId(0),
      isResponse(false),
      isTruncated(false)
{
}

Message::Message()
    : d(new MessagePrivate)
{
}

Message::Message(const Message &other)
    : d(new MessagePrivate)
{
    *this = other;
}

Message &Message::operator=(const Message &other)
{
    *d = *other.d;
    return *this;
}

Message::~Message()
{
    delete d;
}

QHostAddress Message::address() const
{
    return d->address;
}

void Message::setAddress(const QHostAddress &address)
{
    d->address = address;
}

quint16 Message::port() const
{
    return d->port;
}

void Message::setPort(quint16 port)
{
    d->port = port;
}

quint16 Message::transactionId() const
{
    return d->transactionId;
}

void Message::setTransactionId(quint16 transactionId)
{
    d->transactionId = transactionId;
}

bool Message::isResponse() const
{
    return d->isResponse;
}

void Message::setResponse(bool isResponse)
{
    d->isResponse = isResponse;
}

bool Message::isTruncated() const
{
    return d->isTruncated;
}

void Message::setTruncated(bool isTruncated)
{
    d->isTruncated = isTruncated;
}

QList<Query> Message::queries() const
{
    return d->queries;
}

void Message::addQuery(const Query &query)
{
    d->queries.append(query);
}

QList<Record> Message::records() const
{
    return d->records;
}

void Message::addRecord(const Record &record)
{
    // A response to a legacy unicast resolver (RFC 6762 section 6.7) must not
    // set cache-flush and must cap the TTL at 10 seconds.
    Record outgoing = record;
    if (d->legacyUnicastResponse) {
        outgoing.setFlushCache(false);
        outgoing.setTtl(qMin(outgoing.ttl(), quint32(10)));
    }
    d->records.append(outgoing);
}

void Message::reply(const Message &other)
{
    d->legacyUnicastResponse = other.port() != MdnsPort;
    if (d->legacyUnicastResponse) {
        // Legacy unicast replies repeat the question (RFC 6762 section 6.7);
        // addRecord() caps the TTL and clears cache-flush on what follows.
        d->queries = other.queries();
    }
    setInterfaceIndex(other.interfaceIndex());
    setInterfaceGeneration(other.interfaceGeneration());
    if (other.port() == MdnsPort) {
        if (other.address().protocol() == QAbstractSocket::IPv4Protocol) {
            setAddress(MdnsIpv4Address);
        } else {
            setAddress(MdnsIpv6Address);
        }
    } else {
        setAddress(other.address());
    }
    setPort(other.port());
    setTransactionId(other.transactionId());
    setResponse(true);
}

uint Message::interfaceIndex() const { return d->interfaceIndex; }
void Message::setInterfaceIndex(uint index) { d->interfaceIndex = index; }
quint64 Message::interfaceGeneration() const { return d->interfaceGeneration; }
void Message::setInterfaceGeneration(quint64 generation) { d->interfaceGeneration = generation; }

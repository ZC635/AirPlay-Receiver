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

#ifndef QMDNSENGINE_HOSTNAME_P_H
#define QMDNSENGINE_HOSTNAME_P_H

#include <QObject>
#include <QTimer>
#include <QMap>
#include <qmdnsengine/record.h>

class QHostAddress;

namespace QMdnsEngine
{

class AbstractServer;
class Hostname;
class Message;
class Record;

class HostnamePrivate : public QObject
{
    Q_OBJECT

public:

    HostnamePrivate(Hostname *hostname, AbstractServer *server);
    ~HostnamePrivate() override;

    void assertHostname();
    // Reconcile announced address records with the current interfaces:
    // withdraw records that changed, then announce all current records.
    void updateAddressRecords();
    void sendRecords(uint interfaceIndex, const QList<Record> &records, bool goodbye);
    QMap<uint, QList<Record>> announcedRecords;

    AbstractServer *server;

    QByteArray hostnamePrev;
    QByteArray hostname;
    bool hostnameRegistered;
    int hostnameSuffix;

    QTimer registrationTimer;
    QTimer rebroadcastTimer;

private Q_SLOTS:

    void onMessageReceived(const Message &message);
    void onInterfacesChanged();
    void onRegistrationTimeout();
    void onRebroadcastTimeout();

private:

    Hostname *const q;
};

}

#endif // QMDNSENGINE_HOSTNAME_P_H

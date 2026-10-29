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

#ifndef QMDNSENGINE_SERVER_P_H
#define QMDNSENGINE_SERVER_P_H

#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <QMap>
#include <QNetworkInterface>
#include <QSet>

class QHostAddress;

namespace QMdnsEngine
{

class Server;

class ServerPrivate : public QObject
{
    Q_OBJECT

public:

    explicit ServerPrivate(Server *server);
    ~ServerPrivate() override;

    bool bindSocket(QUdpSocket &socket, const QHostAddress &address);
    void refreshInterfaces();
    // Joins the current links if the first poll has not run yet, so a message
    // sent right after construction is not silently dropped.
    void ensureInitialized();
    // Debounced refresh requested by a system interface/address notification.
    void scheduleRefresh();
    void send(const QByteArray &packet, const QHostAddress &destination, quint16 port, uint index);
    uint routeInterface(const QHostAddress &destination) const;
    struct Membership {
        QNetworkInterface iface;
        QHostAddress group;
        QString addresses;
        // Identifies this particular join; replies carrying an older value
        // for the same link were composed before its addresses changed.
        quint64 generation = 0;
    };
    QMap<QString, Membership> members;
    QMap<QString, QString> snapshot;
    // Keys whose join failure has already been reported; retried silently.
    QSet<QString> failedJoins;
    QSet<const QUdpSocket *> failedBinds;
    quint64 generation = 0;
    bool initialized = false;

    QTimer timer;
    QTimer changeTimer;
    // NotifyIpInterfaceChange / NotifyUnicastIpAddressChange handles (Windows).
    void *interfaceNotification = nullptr;
    void *addressNotification = nullptr;
    QUdpSocket ipv4Socket;
    QUdpSocket ipv6Socket;

private Q_SLOTS:

    void onTimeout();
    void onReadyRead();

private:

    Server *const q;
};

}

#endif // QMDNSENGINE_SERVER_P_H

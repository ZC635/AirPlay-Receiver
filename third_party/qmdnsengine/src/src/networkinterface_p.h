#pragma once
#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QHostAddress>

namespace QMdnsEngine {
inline bool usableInterface(const QNetworkInterface &iface) {
    const auto flags = iface.flags();
    return iface.index() > 0 && (flags & QNetworkInterface::IsUp)
        && (flags & QNetworkInterface::IsRunning)
        && (flags & QNetworkInterface::CanMulticast)
        && !(flags & QNetworkInterface::IsLoopBack);
}
inline bool usableAddress(const QHostAddress &address) {
    return !address.isNull() && !address.isLoopback() && !address.isMulticast()
        && address != QHostAddress(QHostAddress::AnyIPv4)
        && address != QHostAddress(QHostAddress::AnyIPv6);
}
}

#pragma once

#include "platform/EnvironmentDiagnostics.h"

#include <QHostAddress>

struct WindowsAddressOperation {
    QHostAddress address;
    int prefixLength = 0;
};

struct WindowsAdapterOperation {
    quint64 luid = 0;
    unsigned int ifType = 0;
    bool enabled = false;
    bool up = false;
    bool tunnel = false;
    bool loopback = false;
    QString description;
    int interfaceMetric = -1;
    QVector<WindowsAddressOperation> addresses;
};

struct WindowsRouteOperation {
    quint64 luid = 0;
    QAbstractSocket::NetworkLayerProtocol family = QAbstractSocket::UnknownNetworkLayerProtocol;
    bool defaultRoute = false;
    int routeMetric = -1;
};

struct WindowsFirewallOperation {
    DiagnosticFact category;
    DiagnosticFact profiles;
    DiagnosticFact executableRule;
};

struct WindowsEnvironmentOperations {
    std::function<QVector<WindowsAdapterOperation>(QDeadlineTimer)> adapters;
    std::function<QVector<WindowsRouteOperation>(QDeadlineTimer)> routes;
    std::function<WindowsFirewallOperation(QDeadlineTimer)> firewall;
};

EnvironmentDiagnosticProviders windowsEnvironmentDiagnosticProviders(
    WindowsEnvironmentOperations operations = {});

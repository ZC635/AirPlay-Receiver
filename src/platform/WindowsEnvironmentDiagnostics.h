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
    bool physicalKnown = false;
    bool physical = false;
    qint64 ipv4InterfaceMetric = -1;
    qint64 ipv6InterfaceMetric = -1;
    QVector<WindowsAddressOperation> addresses;
    bool enabledKnown = false;
};

struct WindowsRouteOperation {
    quint64 luid = 0;
    QAbstractSocket::NetworkLayerProtocol family = QAbstractSocket::UnknownNetworkLayerProtocol;
    bool defaultRoute = false;
    qint64 routeMetric = -1;
};

struct WindowsFirewallOperation {
    DiagnosticFact category;
    DiagnosticFact profiles;
    DiagnosticFact executableRule;
};

QString windowsEnvironmentFirewallScript(QString executable);
QString windowsSystemPowerShellPath(QString systemDirectory);
qint64 windowsRouteMetricFromNative(quint32 metric);

struct WindowsEnvironmentOperations {
    std::function<DiagnosticValue<QVector<WindowsAdapterOperation>>(QDeadlineTimer)> adapters;
    std::function<DiagnosticValue<QVector<WindowsRouteOperation>>(QDeadlineTimer)> routes;
    std::function<WindowsFirewallOperation(QDeadlineTimer)> firewall;
};

EnvironmentDiagnosticProviders windowsEnvironmentDiagnosticProviders(
    WindowsEnvironmentOperations operations = {});

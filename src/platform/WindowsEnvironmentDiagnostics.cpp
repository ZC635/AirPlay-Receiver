#include "platform/WindowsEnvironmentDiagnostics.h"

#include "diagnostics/DiagnosticSanitizer.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSysInfo>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <netioapi.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstring>

namespace {

QVector<WindowsAdapterOperation> readAdapters(QDeadlineTimer) {
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        return {};
    QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
    auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &size) != NO_ERROR)
        return {};
    QVector<WindowsAdapterOperation> result;
    for (IP_ADAPTER_ADDRESSES *entry = addresses; entry; entry = entry->Next) {
        WindowsAdapterOperation adapter;
        adapter.luid = entry->Luid.Value;
        adapter.ifType = entry->IfType;
        adapter.enabled = entry->OperStatus != IfOperStatusDown;
        adapter.up = entry->OperStatus == IfOperStatusUp;
        adapter.tunnel = entry->TunnelType != TUNNEL_TYPE_NONE || entry->IfType == IF_TYPE_TUNNEL;
        adapter.loopback = entry->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        adapter.description = QString::fromWCharArray(entry->Description);
        MIB_IPINTERFACE_ROW row{};
        InitializeIpInterfaceEntry(&row);
        row.Family = AF_INET;
        row.InterfaceLuid = entry->Luid;
        if (GetIpInterfaceEntry(&row) != NO_ERROR) {
            row.Family = AF_INET6;
            GetIpInterfaceEntry(&row);
        }
        if (row.Metric > 0)
            adapter.interfaceMetric = static_cast<int>(row.Metric);
        for (IP_ADAPTER_UNICAST_ADDRESS *address = entry->FirstUnicastAddress; address;
             address = address->Next) {
            const sockaddr *socketAddress = address->Address.lpSockaddr;
            if (!socketAddress)
                continue;
            QHostAddress host;
            if (socketAddress->sa_family == AF_INET) {
                host = QHostAddress(ntohl(reinterpret_cast<const sockaddr_in *>(socketAddress)->sin_addr.s_addr));
            } else if (socketAddress->sa_family == AF_INET6) {
                const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(socketAddress);
                Q_IPV6ADDR raw{};
                std::memcpy(raw.c, &ipv6->sin6_addr, sizeof(raw.c));
                host = QHostAddress(raw);
            }
            if (!host.isNull())
                adapter.addresses.append(WindowsAddressOperation{
                    host, static_cast<int>(address->OnLinkPrefixLength)});
        }
        result.append(std::move(adapter));
    }
    return result;
}

QVector<WindowsRouteOperation> readRoutes(QDeadlineTimer) {
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_UNSPEC, &table) != NO_ERROR || !table)
        return {};
    QVector<WindowsRouteOperation> result;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const MIB_IPFORWARD_ROW2 &row = table->Table[index];
        const bool defaultRoute = row.DestinationPrefix.PrefixLength == 0;
        if (!defaultRoute)
            continue;
        result.append(WindowsRouteOperation{
            row.InterfaceLuid.Value,
            row.DestinationPrefix.Prefix.si_family == AF_INET
                ? QAbstractSocket::IPv4Protocol : QAbstractSocket::IPv6Protocol,
            true, static_cast<int>(row.Metric)});
    }
    FreeMibTable(table);
    return result;
}

DiagnosticFact factFromJson(const QJsonObject &object, const char *name) {
    const QJsonValue value = object.value(QLatin1String(name));
    return value.isString() ? DiagnosticFact::available(value.toString())
                            : DiagnosticFact::unavailable();
}

WindowsFirewallOperation readFirewall(QDeadlineTimer deadline) {
    const int remaining = deadline.remainingTime();
    if (remaining <= 0)
        return {DiagnosticFact::timedOut(), DiagnosticFact::timedOut(), DiagnosticFact::timedOut()};
    const QString executable = QCoreApplication::applicationFilePath();
    QString quotedExecutable = executable;
    quotedExecutable.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString command = QStringLiteral(
        "$ErrorActionPreference='Stop';$target='%1';"
        "$category=(Get-NetConnectionProfile|ForEach-Object{$_.NetworkCategory}|Select-Object -First 1);"
        "$category=switch($category){'DomainAuthenticated'{'domain'};'Private'{'private'};'Public'{'public'};default{$null}};"
        "$profiles=(Get-NetFirewallProfile|ForEach-Object{$n=$_.Name.ToLowerInvariant();$v=if($_.Enabled){'on'}else{'off'};$n+'='+$v}) -join ',';"
        "$rule='absent';$matches=@();Get-NetFirewallRule -PolicyStore ActiveStore|ForEach-Object{$r=$_;Get-NetFirewallApplicationFilter -AssociatedNetFirewallRule $r|ForEach-Object{if($_.Program -ieq $target){$matches+=$r}}};"
        "if($matches.Count -gt 0){$rule=if(($matches|ForEach-Object{$_.Action}) -contains 'Allow'){'confirmed'}else{'denied'}};"
        "[pscustomobject]@{category=$category;profiles=$profiles;rule=$rule}|ConvertTo-Json -Compress")
        .arg(quotedExecutable);
    QProcess process;
    process.start(QStringLiteral("powershell.exe"),
                  {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                   QStringLiteral("-Command"), command});
    if (!process.waitForFinished(remaining)) {
        if (process.error() == QProcess::FailedToStart)
            return {};
        process.kill();
        process.waitForFinished(250);
        return {DiagnosticFact::timedOut(), DiagnosticFact::timedOut(), DiagnosticFact::timedOut()};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput());
    if (!document.isObject())
        return {};
    const QJsonObject object = document.object();
    return {factFromJson(object, "category"), factFromJson(object, "profiles"),
            factFromJson(object, "rule")};
}

QString adapterType(const WindowsAdapterOperation &adapter) {
    if (adapter.tunnel || adapter.ifType == IF_TYPE_TUNNEL)
        return QStringLiteral("tunnel");
    if (adapter.loopback || adapter.ifType == IF_TYPE_SOFTWARE_LOOPBACK)
        return QStringLiteral("loopback");
    if (adapter.ifType == IF_TYPE_ETHERNET_CSMACD)
        return QStringLiteral("ethernet");
    if (adapter.ifType == IF_TYPE_IEEE80211)
        return QStringLiteral("wifi");
    return QStringLiteral("other");
}

QString physicalClass(const WindowsAdapterOperation &adapter) {
    if (adapter.ifType == 0)
        return QStringLiteral("unavailable");
    const QString description = adapter.description.toLower();
    if (adapter.tunnel || adapter.loopback || description.contains(QStringLiteral("virtual")) ||
        description.contains(QStringLiteral("tunnel")))
        return QStringLiteral("virtual");
    return QStringLiteral("physical");
}

EnvironmentDiagnosticProviders defaultProviders() {
    EnvironmentDiagnosticProviders providers;
    providers.operatingSystem = [](QDeadlineTimer) {
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rtlGetVersion = ntdll
            ? reinterpret_cast<LONG (WINAPI *)(OSVERSIONINFOW *)>(
                GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        if (!rtlGetVersion || rtlGetVersion(&version) != 0)
            return DiagnosticFact::unavailable();
        const QString product = version.dwBuildNumber >= 22000
            ? QStringLiteral("Windows 11") : QStringLiteral("Windows 10");
        return DiagnosticFact::available(QStringLiteral("%1 build %2")
            .arg(product).arg(version.dwBuildNumber));
    };
    providers.cpuArchitecture = [](QDeadlineTimer) {
        return DiagnosticFact::available(QSysInfo::currentCpuArchitecture());
    };
    providers.processElevation = [](QDeadlineTimer) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return DiagnosticFact::unavailable();
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
        CloseHandle(token);
        return ok ? DiagnosticFact::available(elevation.TokenIsElevated ? QStringLiteral("elevated")
                                                                         : QStringLiteral("not_elevated"))
                  : DiagnosticFact::unavailable();
    };
    return providers;
}

} // namespace

EnvironmentDiagnosticProviders windowsEnvironmentDiagnosticProviders(WindowsEnvironmentOperations operations) {
    EnvironmentDiagnosticProviders providers = defaultProviders();
    if (!operations.adapters)
        operations.adapters = readAdapters;
    if (!operations.routes)
        operations.routes = readRoutes;
    if (!operations.firewall)
        operations.firewall = readFirewall;
    providers.network = [operations = std::move(operations)](QDeadlineTimer deadline) {
        if (deadline.hasExpired())
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();
        const QVector<WindowsAdapterOperation> inputAdapters = operations.adapters(deadline);
        if (deadline.hasExpired())
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();
        const QVector<WindowsRouteOperation> routes = operations.routes(deadline);
        if (deadline.hasExpired())
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();
        const WindowsFirewallOperation firewall = operations.firewall(deadline);
        if (deadline.hasExpired())
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();

        QVector<WindowsAdapterOperation> sorted = inputAdapters;
        std::sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) {
            return left.luid < right.luid;
        });
        NetworkEnvironmentFact network;
        network.category = firewall.category;
        network.firewallProfiles = firewall.profiles;
        network.executableFirewallRule = firewall.executableRule;
        for (int index = 0; index < sorted.size(); ++index) {
            const WindowsAdapterOperation &source = sorted.at(index);
            NetworkAdapterFact adapter;
            adapter.sessionIndex = index + 1;
            adapter.type = adapterType(source);
            adapter.enabled = source.enabled;
            adapter.up = source.up;
            adapter.physicalClassification = physicalClass(source);
            for (const WindowsRouteOperation &route : routes) {
                if (route.defaultRoute && route.luid == source.luid) {
                    adapter.ownsDefaultRoute = true;
                    if (source.interfaceMetric >= 0 && route.routeMetric >= 0) {
                        const int metric = source.interfaceMetric + route.routeMetric;
                        adapter.routeMetric = adapter.routeMetric < 0
                            ? metric : qMin(adapter.routeMetric, metric);
                    }
                }
            }
            for (const WindowsAddressOperation &address : source.addresses)
                adapter.prefixes.append(DiagnosticSanitizer::maskedAddress(address.address, address.prefixLength));
            network.adapters.append(std::move(adapter));
        }
        return DiagnosticValue<NetworkEnvironmentFact>::available(network);
    };
    return providers;
}

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
#include <limits>

namespace {

DiagnosticValue<QVector<WindowsAdapterOperation>> readAdapters(QDeadlineTimer deadline) {
    if (deadline.hasExpired())
        return DiagnosticValue<QVector<WindowsAdapterOperation>>::timedOut();
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        return DiagnosticValue<QVector<WindowsAdapterOperation>>::unavailable();
    if (deadline.hasExpired())
        return DiagnosticValue<QVector<WindowsAdapterOperation>>::timedOut();
    QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
    auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &size) != NO_ERROR)
        return DiagnosticValue<QVector<WindowsAdapterOperation>>::unavailable();
    if (deadline.hasExpired())
        return DiagnosticValue<QVector<WindowsAdapterOperation>>::timedOut();
    QVector<WindowsAdapterOperation> result;
    for (IP_ADAPTER_ADDRESSES *entry = addresses; entry; entry = entry->Next) {
        if (deadline.hasExpired())
            return DiagnosticValue<QVector<WindowsAdapterOperation>>::timedOut();
        WindowsAdapterOperation adapter;
        adapter.luid = entry->Luid.Value;
        adapter.ifType = entry->IfType;
        adapter.up = entry->OperStatus == IfOperStatusUp;
        adapter.tunnel = entry->TunnelType != TUNNEL_TYPE_NONE || entry->IfType == IF_TYPE_TUNNEL;
        adapter.loopback = entry->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        MIB_IF_ROW2 ifRow{};
        ifRow.InterfaceLuid = entry->Luid;
        if (GetIfEntry2(&ifRow) == NO_ERROR) {
            adapter.enabled = ifRow.AdminStatus == NET_IF_ADMIN_STATUS_UP;
            adapter.enabledKnown = true;
            adapter.physicalKnown = true;
            adapter.physical = ifRow.InterfaceAndOperStatusFlags.HardwareInterface != 0;
        }
        for (ADDRESS_FAMILY family : {AF_INET, AF_INET6}) {
            if (deadline.hasExpired())
                return DiagnosticValue<QVector<WindowsAdapterOperation>>::timedOut();
            MIB_IPINTERFACE_ROW row{};
            InitializeIpInterfaceEntry(&row);
            row.Family = family;
            row.InterfaceLuid = entry->Luid;
            if (GetIpInterfaceEntry(&row) == NO_ERROR) {
                if (family == AF_INET)
                    adapter.ipv4InterfaceMetric = static_cast<qint64>(row.Metric);
                else
                    adapter.ipv6InterfaceMetric = static_cast<qint64>(row.Metric);
            }
        }
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
    return DiagnosticValue<QVector<WindowsAdapterOperation>>::available(std::move(result));
}

DiagnosticValue<QVector<WindowsRouteOperation>> readRoutes(QDeadlineTimer deadline) {
    if (deadline.hasExpired())
        return DiagnosticValue<QVector<WindowsRouteOperation>>::timedOut();
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_UNSPEC, &table) != NO_ERROR || !table)
        return DiagnosticValue<QVector<WindowsRouteOperation>>::unavailable();
    if (deadline.hasExpired()) {
        FreeMibTable(table);
        return DiagnosticValue<QVector<WindowsRouteOperation>>::timedOut();
    }
    QVector<WindowsRouteOperation> result;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        if (deadline.hasExpired()) {
            FreeMibTable(table);
            return DiagnosticValue<QVector<WindowsRouteOperation>>::timedOut();
        }
        const MIB_IPFORWARD_ROW2 &row = table->Table[index];
        const bool defaultRoute = row.DestinationPrefix.PrefixLength == 0;
        if (!defaultRoute)
            continue;
        result.append(WindowsRouteOperation{
            row.InterfaceLuid.Value,
            row.DestinationPrefix.Prefix.si_family == AF_INET
                ? QAbstractSocket::IPv4Protocol : QAbstractSocket::IPv6Protocol,
            true, windowsRouteMetricFromNative(row.Metric)});
    }
    FreeMibTable(table);
    return DiagnosticValue<QVector<WindowsRouteOperation>>::available(std::move(result));
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
    wchar_t systemDirectory[MAX_PATH];
    const UINT systemDirectoryLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    const QString powerShell = systemDirectoryLength == 0 || systemDirectoryLength >= MAX_PATH
        ? QString() : windowsSystemPowerShellPath(
            QString::fromWCharArray(systemDirectory, static_cast<int>(systemDirectoryLength)));
    if (powerShell.isEmpty())
        return {};
    const QString command = windowsEnvironmentFirewallScript(QCoreApplication::applicationFilePath());
    QProcess process;
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    process.start(powerShell,
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
    if (adapter.physicalKnown)
        return adapter.physical ? QStringLiteral("physical") : QStringLiteral("virtual");
    if (adapter.tunnel || adapter.loopback)
        return QStringLiteral("virtual");
    return QStringLiteral("unavailable");
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

QString windowsEnvironmentFirewallScript(QString executable) {
    executable.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral(
        "$ErrorActionPreference='Stop';$target='%1';"
        "$categories=@(Get-NetConnectionProfile|ForEach-Object{switch($_.NetworkCategory){'DomainAuthenticated'{'domain'};'Private'{'private'};'Public'{'public'}}}|Where-Object{$_}|Select-Object -Unique);"
        "$category=$categories|Select-Object -First 1;"
        "$activeProfiles=@($categories|ForEach-Object{switch($_){'domain'{'Domain'};'private'{'Private'};'public'{'Public'}}});"
        "$profiles=(Get-NetFirewallProfile|ForEach-Object{$n=$_.Name.ToLowerInvariant();$v=if($_.Enabled){'on'}else{'off'};$n+'='+$v}) -join ',';"
        "$rule='not_read';if($activeProfiles.Count -gt 0){try{$candidateRules=@();Get-NetFirewallRule -PolicyStore ActiveStore|ForEach-Object{$r=$_;$ruleProfiles=@($r.Profile.ToString().Split(',')|ForEach-Object{$_.Trim()});$profileApplies=($ruleProfiles -contains 'Any') -or (($ruleProfiles|Where-Object{$activeProfiles -contains $_}).Count -gt 0);if($r.Enabled.ToString() -eq 'True' -and $r.Direction.ToString() -eq 'Inbound' -and $profileApplies){Get-NetFirewallApplicationFilter -AssociatedNetFirewallRule $r|ForEach-Object{if($_.Program -ieq $target){$candidateRules+=$r}}}};"
        "if($candidateRules.Count -eq 0){$rule='absent'}elseif(($candidateRules|ForEach-Object{$_.Action}) -contains 'Block'){$rule='denied'}else{$rule='confirmed'}}catch{$rule='not_read'}};"
        "[pscustomobject]@{category=$category;profiles=$profiles;rule=$rule}|ConvertTo-Json -Compress")
        .arg(executable);
}

QString windowsSystemPowerShellPath(QString systemDirectory) {
    if (systemDirectory.isEmpty())
        return {};
    while (systemDirectory.endsWith(QLatin1Char('\\')) || systemDirectory.endsWith(QLatin1Char('/')))
        systemDirectory.chop(1);
    return systemDirectory + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
}

qint64 windowsRouteMetricFromNative(quint32 metric) {
    return static_cast<qint64>(metric);
}

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
        const auto inputAdapters = operations.adapters(deadline);
        if (deadline.hasExpired())
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();
        if (inputAdapters.status != DiagnosticFactStatus::Available)
            return DiagnosticValue<NetworkEnvironmentFact>{inputAdapters.status, {}};
        const auto routes = operations.routes(deadline);
        if (deadline.hasExpired())
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();
        if (routes.status != DiagnosticFactStatus::Available)
            return DiagnosticValue<NetworkEnvironmentFact>{routes.status, {}};
        QVector<WindowsAdapterOperation> sorted = inputAdapters.value;
        std::sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) {
            return left.luid < right.luid;
        });
        NetworkEnvironmentFact network;
        for (int index = 0; index < sorted.size(); ++index) {
            const WindowsAdapterOperation &source = sorted.at(index);
            NetworkAdapterFact adapter;
            adapter.sessionIndex = index + 1;
            adapter.type = adapterType(source);
            adapter.enabled = source.enabled;
            adapter.enabledKnown = source.enabledKnown;
            adapter.up = source.up;
            adapter.physicalClassification = physicalClass(source);
            for (const WindowsRouteOperation &route : routes.value) {
                if (route.defaultRoute && route.luid == source.luid) {
                    adapter.ownsDefaultRoute = true;
                    const qint64 interfaceMetric = route.family == QAbstractSocket::IPv4Protocol
                        ? source.ipv4InterfaceMetric : source.ipv6InterfaceMetric;
                    if (interfaceMetric >= 0 && route.routeMetric >= 0 &&
                        interfaceMetric <= std::numeric_limits<qint64>::max() - route.routeMetric) {
                        const qint64 metric = interfaceMetric + route.routeMetric;
                        adapter.routeMetric = adapter.routeMetric < 0
                            ? metric : qMin(adapter.routeMetric, metric);
                    }
                }
            }
            for (const WindowsAddressOperation &address : source.addresses)
                adapter.prefixes.append(DiagnosticSanitizer::maskedAddress(address.address, address.prefixLength));
            network.adapters.append(std::move(adapter));
        }
        if (deadline.hasExpired())
            return DiagnosticValue<NetworkEnvironmentFact>{DiagnosticFactStatus::TimedOut,
                                                            std::move(network)};
        const WindowsFirewallOperation firewall = operations.firewall(deadline);
        network.category = firewall.category;
        network.firewallProfiles = firewall.profiles;
        network.executableFirewallRule = firewall.executableRule;
        return DiagnosticValue<NetworkEnvironmentFact>{
            deadline.hasExpired() ? DiagnosticFactStatus::TimedOut : DiagnosticFactStatus::Available,
            std::move(network)};
    };
    return providers;
}

#include <QtTest/QtTest>

#include <QHostAddress>
#include <QThread>

#include <algorithm>

#include "platform/EnvironmentDiagnostics.h"
#include "platform/WindowsEnvironmentDiagnostics.h"

namespace {

EnvironmentDiagnosticProviders fakeProviders() {
    EnvironmentDiagnosticProviders providers;
    providers.operatingSystem = [](QDeadlineTimer) {
        return DiagnosticFact::available(QStringLiteral("Windows 11 build 26100"));
    };
    providers.cpuArchitecture = [](QDeadlineTimer) {
        return DiagnosticFact::available(QStringLiteral("x86_64"));
    };
    providers.processElevation = [](QDeadlineTimer) {
        return DiagnosticFact::available(QStringLiteral("not_elevated"));
    };
    providers.network = [](QDeadlineTimer) {
        NetworkEnvironmentFact network;
        network.category = DiagnosticFact::available(QStringLiteral("private"));
        network.firewallProfiles = DiagnosticFact::available(
            QStringLiteral("domain=on,private=on,public=on"));
        network.executableFirewallRule = DiagnosticFact::available(QStringLiteral("confirmed"));
        network.adapters = {{1, QStringLiteral("ethernet"), true, true,
                             QStringLiteral("physical"), 25, true,
                             {QStringLiteral("192.168.10.xxx/24"),
                              QStringLiteral("fd12:3456:789a:bcde:xxxx/64")}}};
        return DiagnosticValue<NetworkEnvironmentFact>::available(network);
    };
    return providers;
}

bool containsForbiddenData(const QList<DiagnosticEvent> &events) {
    const QStringList forbidden = {
        QStringLiteral("192.168.10.22"), QStringLiteral("fd12:3456:789a:bcde::42"),
        QStringLiteral("203.0.113.9"), QStringLiteral("Cafe SSID"),
        QStringLiteral("Alice"), QStringLiteral("Workstation-77"),
        QStringLiteral("aa:bb:cc:dd:ee:ff"), QStringLiteral("C:\\\\Users\\\\Alice")};
    for (const DiagnosticEvent &event : events) {
        const QString line = event.name + QLatin1Char(' ') +
            event.fields.values().join(QLatin1Char(' '));
        for (const QString &value : forbidden) {
            if (line.contains(value, Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}

} // namespace

class EnvironmentDiagnosticsTest : public QObject {
    Q_OBJECT

private slots:
    void collectionPolicyRequiresActiveDiagnosticSession() {
        QVERIFY(!shouldCollectEnvironmentDiagnostics(false));
        QVERIFY(shouldCollectEnvironmentDiagnostics(true));
    }

    void collectsFakeProviderSnapshot() {
        const EnvironmentSnapshot snapshot = EnvironmentDiagnostics::collect(fakeProviders(), 3000);

        QCOMPARE(snapshot.operatingSystem.value, QStringLiteral("Windows 11 build 26100"));
        QCOMPARE(snapshot.cpuArchitecture.value, QStringLiteral("x86_64"));
        QCOMPARE(snapshot.processElevation.value, QStringLiteral("not_elevated"));
        QCOMPARE(snapshot.network.value.adapters.size(), 1);
        const NetworkAdapterFact adapter = snapshot.network.value.adapters.constFirst();
        QCOMPARE(adapter.sessionIndex, 1);
        QCOMPARE(adapter.prefixes, QStringList({QStringLiteral("192.168.10.xxx/24"),
                                                QStringLiteral("fd12:3456:789a:bcde:xxxx/64")}));
    }

    void toleratesUnavailableAndTimedOutProviders() {
        EnvironmentDiagnosticProviders providers = fakeProviders();
        providers.operatingSystem = [](QDeadlineTimer) { return DiagnosticFact::unavailable(); };
        providers.network = [](QDeadlineTimer) {
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();
        };

        const EnvironmentSnapshot snapshot = EnvironmentDiagnostics::collect(providers, 3000);

        QCOMPARE(snapshot.operatingSystem.status, DiagnosticFactStatus::Unavailable);
        QCOMPARE(snapshot.network.status, DiagnosticFactStatus::TimedOut);
    }

    void eventsMaskForbiddenProviderData() {
        EnvironmentDiagnosticProviders providers = fakeProviders();
        providers.network = [](QDeadlineTimer) {
            NetworkEnvironmentFact network;
            network.category = DiagnosticFact::available(QStringLiteral("Cafe SSID Alice Workstation-77"));
            network.firewallProfiles = DiagnosticFact::available(QStringLiteral("aa:bb:cc:dd:ee:ff"));
            network.executableFirewallRule = DiagnosticFact::available(QStringLiteral("C:\\Users\\Alice\\receiver.exe"));
            network.adapters = {{8, QStringLiteral("wifi C:\\Users\\Alice"), true, true,
                                 QStringLiteral("physical aa:bb:cc:dd:ee:ff"), 10, true,
                                 {QStringLiteral("192.168.10.22/24"),
                                  QStringLiteral("fd12:3456:789a:bcde::42/64"),
                                  QStringLiteral("203.0.113.9/24")}}};
            return DiagnosticValue<NetworkEnvironmentFact>::available(network);
        };

        QVERIFY(!containsForbiddenData(EnvironmentDiagnostics::events(
            EnvironmentDiagnostics::collect(providers, 3000))));
    }

    void eventsPreserveAlreadyMaskedPrefixesAndPublicFamilies() {
        const QList<DiagnosticEvent> events = EnvironmentDiagnostics::events(
            EnvironmentDiagnostics::collect(fakeProviders(), 3000));
        const DiagnosticEvent adapter = events.constLast();

        QCOMPARE(adapter.fields.value(QStringLiteral("prefixes")),
                 QStringLiteral("192.168.10.xxx/24,fd12:3456:789a:bcde:xxxx/64"));

        EnvironmentSnapshot publicSnapshot = EnvironmentDiagnostics::collect(fakeProviders(), 3000);
        publicSnapshot.network.value.adapters.first().prefixes = {
            QStringLiteral("public_ipv4"), QStringLiteral("public_ipv6")};
        const DiagnosticEvent publicAdapter = EnvironmentDiagnostics::events(publicSnapshot).constLast();
        QCOMPARE(publicAdapter.fields.value(QStringLiteral("prefixes")),
                 QStringLiteral("public_ipv4,public_ipv6"));

        EnvironmentSnapshot rawSnapshot = EnvironmentDiagnostics::collect(fakeProviders(), 3000);
        rawSnapshot.network.value.adapters.first().prefixes = {
            QStringLiteral("192.168.10.22/24"), QStringLiteral("fd12:3456:789a:bcde::42/64"),
            QStringLiteral("not_an_address")};
        const DiagnosticEvent rawAdapter = EnvironmentDiagnostics::events(rawSnapshot).constLast();
        QCOMPARE(rawAdapter.fields.value(QStringLiteral("prefixes")),
                 QStringLiteral("192.168.10.xxx/24,fd12:3456:789a:bcde:xxxx/64,unavailable"));

        EnvironmentSnapshot invalidSnapshot = EnvironmentDiagnostics::collect(fakeProviders(), 3000);
        invalidSnapshot.network.value.adapters.first().prefixes = {
            QStringLiteral("999.xxx.xxx.xxx/8"), QStringLiteral("203.0.113.xxx/24"),
            QStringLiteral("10.xxx.xxx/8"), QStringLiteral("10.0.xxx.xxx/8"),
            QStringLiteral("fd12:3456:xxxx/64"),
            QStringLiteral("fd12:3456:789a:bcde:0000:xxxx/64")};
        const DiagnosticEvent invalidAdapter = EnvironmentDiagnostics::events(invalidSnapshot).constLast();
        QCOMPARE(invalidAdapter.fields.value(QStringLiteral("prefixes")),
                 QStringLiteral("unavailable,unavailable,unavailable,unavailable,unavailable,unavailable"));

        EnvironmentSnapshot legitimateSnapshot = EnvironmentDiagnostics::collect(fakeProviders(), 3000);
        legitimateSnapshot.network.value.adapters.first().prefixes = {
            QStringLiteral("10.xxx.xxx.xxx/8"), QStringLiteral("172.16.xxx.xxx/16"),
            QStringLiteral("192.168.10.xxx/24"), QStringLiteral("fd12:3456:789a:bcde:xxxx/64"),
            QStringLiteral("fe80:0000:0000:0000:xxxx/64"), QStringLiteral("public_ipv4"),
            QStringLiteral("public_ipv6")};
        const DiagnosticEvent legitimateAdapter = EnvironmentDiagnostics::events(legitimateSnapshot).constLast();
        QCOMPARE(legitimateAdapter.fields.value(QStringLiteral("prefixes")),
                 QStringLiteral("10.xxx.xxx.xxx/8,172.16.xxx.xxx/16,192.168.10.xxx/24,"
                                "fd12:3456:789a:bcde:xxxx/64,fe80:0000:0000:0000:xxxx/64,"
                                "public_ipv4,public_ipv6"));
    }

    void mapsInjectedWindowsOperationsWithoutSystemCalls() {
        WindowsEnvironmentOperations operations;
        operations.adapters = [](QDeadlineTimer) {
            WindowsAdapterOperation ethernet{20, 6, true, true, false, false, true, true, 10, 10,
                                              {{QHostAddress(QStringLiteral("192.168.10.22")), 24}}};
            WindowsAdapterOperation wifi{40, 71, true, false, false, false, true, true, 30, 30,
                                          {{QHostAddress(QStringLiteral("fd12:3456:789a:bcde::42")), 64}}};
            WindowsAdapterOperation tunnel{60, 24, true, true, true, false, false, false, 20, 20, {}};
            WindowsAdapterOperation loopback{70, 24, true, true, false, true, false, false, 1, 1, {}};
            WindowsAdapterOperation virtualAdapter{90, 71, true, true, true, false, true, false, 5, 5,
                                                    {{QHostAddress(QStringLiteral("203.0.113.9")), 24}}};
            return DiagnosticValue<QVector<WindowsAdapterOperation>>::available(
                {virtualAdapter, ethernet, wifi, tunnel, loopback});
        };
        operations.routes = [](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsRouteOperation>>::available({
                {20, QAbstractSocket::IPv4Protocol, true, 15},
                {40, QAbstractSocket::IPv6Protocol, true, 20}});
        };
        operations.firewall = [](QDeadlineTimer) {
            return WindowsFirewallOperation{DiagnosticFact::available(QStringLiteral("private")),
                                            DiagnosticFact::available(QStringLiteral("private=on")),
                                            DiagnosticFact::timedOut()};
        };

        const EnvironmentSnapshot snapshot = EnvironmentDiagnostics::collect(
            windowsEnvironmentDiagnosticProviders(operations), 3000);
        const QVector<NetworkAdapterFact> adapters = snapshot.network.value.adapters;

        QCOMPARE(adapters.size(), 5);
        QCOMPARE(adapters.at(0).sessionIndex, 1);
        QCOMPARE(adapters.at(0).type, QStringLiteral("ethernet"));
        QCOMPARE(adapters.at(0).routeMetric, 25);
        QVERIFY(adapters.at(0).ownsDefaultRoute);
        QCOMPARE(adapters.at(1).type, QStringLiteral("wifi"));
        QCOMPARE(adapters.at(1).routeMetric, 50);
        QVERIFY(adapters.at(1).ownsDefaultRoute);
        QCOMPARE(adapters.at(2).type, QStringLiteral("tunnel"));
        QCOMPARE(adapters.at(3).type, QStringLiteral("loopback"));
        QCOMPARE(adapters.at(4).physicalClassification, QStringLiteral("virtual"));
        QCOMPARE(adapters.at(4).prefixes, QStringList({QStringLiteral("public_ipv4")}));

        const QList<DiagnosticEvent> events = EnvironmentDiagnostics::events(snapshot);
        QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const DiagnosticEvent &event) {
            return event.fields.value(QStringLiteral("firewall_rule")) == QStringLiteral("timed_out");
        }));
    }

    void windowsOperationsPreserveFailuresAndFamilySpecificMetrics() {
        WindowsEnvironmentOperations operations;
        operations.adapters = [](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsAdapterOperation>>::unavailable();
        };
        operations.routes = [](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsRouteOperation>>::available({});
        };
        operations.firewall = [](QDeadlineTimer) { return WindowsFirewallOperation{}; };
        QCOMPARE(EnvironmentDiagnostics::collect(windowsEnvironmentDiagnosticProviders(operations), 3000)
                     .network.status, DiagnosticFactStatus::Unavailable);

        WindowsAdapterOperation adapter;
        adapter.luid = 1;
        adapter.ifType = 6;
        adapter.enabled = false;
        adapter.up = true;
        adapter.ipv4InterfaceMetric = 10;
        adapter.ipv6InterfaceMetric = 50;
        operations.adapters = [adapter](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsAdapterOperation>>::available({adapter});
        };
        operations.routes = [](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsRouteOperation>>::available({
                {1, QAbstractSocket::IPv4Protocol, true, 5},
                {1, QAbstractSocket::IPv6Protocol, true, 2}});
        };
        const EnvironmentSnapshot snapshot = EnvironmentDiagnostics::collect(
            windowsEnvironmentDiagnosticProviders(operations), 3000);
        QCOMPARE(snapshot.network.status, DiagnosticFactStatus::Available);
        QCOMPARE(snapshot.network.value.adapters.constFirst().routeMetric, 15);
        QVERIFY(!snapshot.network.value.adapters.constFirst().enabled);
        QVERIFY(snapshot.network.value.adapters.constFirst().up);

        operations.routes = [](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsRouteOperation>>::timedOut();
        };
        QCOMPARE(EnvironmentDiagnostics::collect(windowsEnvironmentDiagnosticProviders(operations), 3000)
                     .network.status, DiagnosticFactStatus::TimedOut);
    }

    void firewallScriptUsesExplicitCandidatesAndEnabledRules() {
        const QString script = windowsEnvironmentFirewallScript(
            QStringLiteral("C:\\Program Files\\AirPlay\\receiver.exe"));

        QVERIFY(script.contains(QStringLiteral("$candidateRules")));
        QVERIFY(!script.contains(QStringLiteral("$matches"), Qt::CaseInsensitive));
        QVERIFY(script.contains(QStringLiteral("$r.Enabled.ToString() -eq 'True'")));
        QVERIFY(!script.contains(QStringLiteral("-match $profile")));
        QVERIFY(script.contains(QStringLiteral("-contains 'Block'")));
        QVERIFY(script.contains(QStringLiteral("$activeProfiles")));
        QVERIFY(script.contains(QStringLiteral("Split(',')|ForEach-Object{$_.Trim()}")));
        QVERIFY(script.contains(QStringLiteral("$activeProfiles.Count -gt 0")));
    }

    void mixedNetworkCategoriesUseDeterministicPrecedence() {
        QCOMPARE(windowsNetworkCategoryForProfiles(
                     {QStringLiteral("private"), QStringLiteral("public")}),
                 QStringLiteral("public"));
        QCOMPARE(windowsNetworkCategoryForProfiles(
                     {QStringLiteral("public"), QStringLiteral("private")}),
                 QStringLiteral("public"));
        QCOMPARE(windowsNetworkCategoryForProfiles(
                     {QStringLiteral("private"), QStringLiteral("domain")}),
                 QStringLiteral("domain"));
        QCOMPARE(windowsNetworkCategoryForProfiles(
                     {QStringLiteral("domain"), QStringLiteral("private")}),
                 QStringLiteral("domain"));
        QCOMPARE(windowsNetworkCategoryForProfiles({QStringLiteral("private")}),
                 QStringLiteral("private"));
        QVERIFY(windowsNetworkCategoryForProfiles({}).isEmpty());

        const QString script = windowsEnvironmentFirewallScript(QStringLiteral("receiver.exe"));
        QVERIFY(!script.contains(QStringLiteral("$category=$categories|Select-Object -First 1")));
        QVERIFY(script.contains(QStringLiteral("$categories -contains 'public'")));
        QVERIFY(script.contains(QStringLiteral("$categories -contains 'domain'")));
    }

    void eventsReportUnknownEnabledAndRouteMetricHonestly() {
        EnvironmentSnapshot snapshot = EnvironmentDiagnostics::collect(fakeProviders(), 3000);
        NetworkAdapterFact &adapter = snapshot.network.value.adapters.first();
        adapter.enabledKnown = false;
        adapter.routeMetric = -1;
        DiagnosticEvent event = EnvironmentDiagnostics::events(snapshot).constLast();
        QCOMPARE(event.fields.value(QStringLiteral("enabled")), QStringLiteral("unavailable"));
        QCOMPARE(event.fields.value(QStringLiteral("route_metric")), QStringLiteral("unavailable"));

        adapter.enabledKnown = true;
        adapter.enabled = false;
        adapter.routeMetric = 6000000000LL;
        event = EnvironmentDiagnostics::events(snapshot).constLast();
        QCOMPARE(event.fields.value(QStringLiteral("enabled")), QStringLiteral("no"));
        QCOMPARE(event.fields.value(QStringLiteral("route_metric")), QStringLiteral("6000000000"));
    }

    void windowsOperationsKeepWideMetricsAndMissingProvidersAreUnavailable() {
        WindowsEnvironmentOperations operations;
        WindowsAdapterOperation adapter;
        adapter.luid = 1;
        adapter.ifType = 6;
        adapter.enabledKnown = true;
        adapter.enabled = true;
        adapter.ipv4InterfaceMetric = 3000000000LL;
        operations.adapters = [adapter](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsAdapterOperation>>::available({adapter});
        };
        operations.routes = [](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsRouteOperation>>::available(
                {{1, QAbstractSocket::IPv4Protocol, true, 3000000000LL}});
        };
        operations.firewall = [](QDeadlineTimer) { return WindowsFirewallOperation{}; };
        const auto collected = EnvironmentDiagnostics::collect(
            windowsEnvironmentDiagnosticProviders(operations), 3000);
        QCOMPARE(collected.network.value.adapters.constFirst().routeMetric, 6000000000LL);

        adapter.ipv4InterfaceMetric = -1;
        operations.adapters = [adapter](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsAdapterOperation>>::available({adapter});
        };
        const auto unknownMetric = EnvironmentDiagnostics::collect(
            windowsEnvironmentDiagnosticProviders(operations), 3000);
        QVERIFY(unknownMetric.network.value.adapters.constFirst().ownsDefaultRoute);
        QCOMPARE(unknownMetric.network.value.adapters.constFirst().routeMetric, -1LL);
        const DiagnosticEvent unknownMetricEvent = EnvironmentDiagnostics::events(unknownMetric).constLast();
        QCOMPARE(unknownMetricEvent.fields.value(QStringLiteral("default_route")), QStringLiteral("yes"));
        QCOMPARE(unknownMetricEvent.fields.value(QStringLiteral("route_metric")),
                 QStringLiteral("unavailable"));

        const EnvironmentSnapshot missing = EnvironmentDiagnostics::collect({}, 100);
        QCOMPARE(missing.operatingSystem.status, DiagnosticFactStatus::Unavailable);
        QCOMPARE(missing.cpuArchitecture.status, DiagnosticFactStatus::Unavailable);
        QCOMPARE(missing.processElevation.status, DiagnosticFactStatus::Unavailable);
        QCOMPARE(missing.network.status, DiagnosticFactStatus::Unavailable);
    }

    void buildsSystemPowerShellPathWithoutPathLookup() {
        QCOMPARE(windowsSystemPowerShellPath(QStringLiteral("C:\\Windows\\System32")),
                 QStringLiteral("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"));
        QVERIFY(windowsSystemPowerShellPath({}).isEmpty());
    }

    void preservesNativeUnsignedRouteMetricWidth() {
        QCOMPARE(windowsRouteMetricFromNative(3000000000U), 3000000000LL);
    }

    void timedOutFirewallPreservesCollectedNetworkDetails() {
        WindowsEnvironmentOperations operations;
        WindowsAdapterOperation adapter;
        adapter.luid = 1;
        adapter.ifType = 6;
        adapter.enabled = true;
        adapter.up = true;
        adapter.physicalKnown = true;
        adapter.physical = true;
        operations.adapters = [adapter](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsAdapterOperation>>::available({adapter});
        };
        operations.routes = [](QDeadlineTimer) {
            return DiagnosticValue<QVector<WindowsRouteOperation>>::available({});
        };
        operations.firewall = [](QDeadlineTimer) {
            QThread::msleep(40);
            return WindowsFirewallOperation{DiagnosticFact::timedOut(), DiagnosticFact::timedOut(),
                                            DiagnosticFact::timedOut()};
        };

        const EnvironmentSnapshot snapshot = EnvironmentDiagnostics::collect(
            windowsEnvironmentDiagnosticProviders(operations), 20);
        QCOMPARE(snapshot.network.status, DiagnosticFactStatus::TimedOut);
        QCOMPARE(snapshot.network.value.adapters.size(), 1);
        const QList<DiagnosticEvent> events = EnvironmentDiagnostics::events(snapshot);
        QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const DiagnosticEvent &event) {
            return event.fields.value(QStringLiteral("firewall_rule")) == QStringLiteral("timed_out");
        }));
        QCOMPARE(events.constLast().name, QStringLiteral("environment_adapter"));

        EnvironmentDiagnosticProviders emptyTimeout = fakeProviders();
        emptyTimeout.network = [](QDeadlineTimer) {
            return DiagnosticValue<NetworkEnvironmentFact>::timedOut();
        };
        QCOMPARE(EnvironmentDiagnostics::events(EnvironmentDiagnostics::collect(emptyTimeout, 3000)).size(), 1);
    }

    void collectionSharesDeadlineAndSkipsProvidersAfterExpiry() {
        QList<qint64> endpoints;
        QList<qint64> remaining;
        EnvironmentDiagnosticProviders providers;
        providers.operatingSystem = [&](QDeadlineTimer deadline) {
            endpoints.append(deadline.deadline());
            remaining.append(deadline.remainingTime());
            return DiagnosticFact::available(QStringLiteral("Windows 11 build 26100"));
        };
        providers.cpuArchitecture = [&](QDeadlineTimer deadline) {
            endpoints.append(deadline.deadline());
            remaining.append(deadline.remainingTime());
            return DiagnosticFact::available(QStringLiteral("x86_64"));
        };
        providers.processElevation = [&](QDeadlineTimer deadline) {
            endpoints.append(deadline.deadline());
            remaining.append(deadline.remainingTime());
            return DiagnosticFact::available(QStringLiteral("not_elevated"));
        };
        providers.network = [&](QDeadlineTimer deadline) {
            endpoints.append(deadline.deadline());
            remaining.append(deadline.remainingTime());
            return DiagnosticValue<NetworkEnvironmentFact>::available({});
        };
        EnvironmentDiagnostics::collect(providers, 100);
        QCOMPARE(endpoints.size(), 4);
        QCOMPARE(endpoints.at(0), endpoints.at(1));
        QCOMPARE(endpoints.at(1), endpoints.at(2));
        QCOMPARE(endpoints.at(2), endpoints.at(3));
        QVERIFY(remaining.at(0) >= remaining.at(1));
        QVERIFY(remaining.at(1) >= remaining.at(2));
        QVERIFY(remaining.at(2) >= remaining.at(3));

        int laterCalls = 0;
        providers.operatingSystem = [](QDeadlineTimer) {
            QThread::msleep(20);
            return DiagnosticFact::available(QStringLiteral("Windows 11 build 26100"));
        };
        providers.cpuArchitecture = [&](QDeadlineTimer) { ++laterCalls; return DiagnosticFact::available({}); };
        providers.processElevation = [&](QDeadlineTimer) { ++laterCalls; return DiagnosticFact::available({}); };
        providers.network = [&](QDeadlineTimer) { ++laterCalls; return DiagnosticValue<NetworkEnvironmentFact>::available({}); };
        const EnvironmentSnapshot expired = EnvironmentDiagnostics::collect(providers, 5);
        QCOMPARE(expired.operatingSystem.status, DiagnosticFactStatus::TimedOut);
        QCOMPARE(expired.cpuArchitecture.status, DiagnosticFactStatus::TimedOut);
        QCOMPARE(expired.processElevation.status, DiagnosticFactStatus::TimedOut);
        QCOMPARE(expired.network.status, DiagnosticFactStatus::TimedOut);
        QCOMPARE(laterCalls, 0);
    }
};

QTEST_GUILESS_MAIN(EnvironmentDiagnosticsTest)
#include "EnvironmentDiagnosticsTest.moc"

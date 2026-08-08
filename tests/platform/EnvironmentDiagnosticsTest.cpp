#include <QtTest/QtTest>

#include <QHostAddress>

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

    void mapsInjectedWindowsOperationsWithoutSystemCalls() {
        WindowsEnvironmentOperations operations;
        operations.adapters = [](QDeadlineTimer) {
            return QVector<WindowsAdapterOperation>{
                {90, 71, true, true, false, false, QStringLiteral("virtual tunnel"), 5,
                 {{QHostAddress(QStringLiteral("203.0.113.9")), 24}}},
                {20, 6, true, true, false, false, QStringLiteral("ethernet"), 10,
                 {{QHostAddress(QStringLiteral("192.168.10.22")), 24}}},
                {40, 71, true, false, false, false, QStringLiteral("wifi"), 30,
                 {{QHostAddress(QStringLiteral("fd12:3456:789a:bcde::42")), 64}}},
                {60, 24, true, true, true, false, QStringLiteral("tunnel"), 20, {}},
                {70, 24, true, true, false, true, QStringLiteral("loopback"), 1, {}}};
        };
        operations.routes = [](QDeadlineTimer) {
            return QVector<WindowsRouteOperation>{
                {20, QAbstractSocket::IPv4Protocol, true, 15},
                {40, QAbstractSocket::IPv6Protocol, true, 20}};
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
};

QTEST_GUILESS_MAIN(EnvironmentDiagnosticsTest)
#include "EnvironmentDiagnosticsTest.moc"

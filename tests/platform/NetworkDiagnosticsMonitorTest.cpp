#include "platform/NetworkDiagnosticsMonitor.h"

#include "support/CollectingDiagnosticLogSink.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>

namespace {

NetworkEnvironmentFact networkFact(const QStringList &prefixes) {
    NetworkAdapterFact adapter;
    adapter.sessionIndex = 1;
    adapter.type = QStringLiteral("ethernet");
    adapter.enabledKnown = true;
    adapter.enabled = true;
    adapter.up = true;
    adapter.physicalClassification = QStringLiteral("physical");
    adapter.routeMetric = 10;
    adapter.ownsDefaultRoute = true;
    adapter.prefixes = prefixes;
    NetworkEnvironmentFact network;
    network.adapters = {adapter};
    network.category = DiagnosticFact::available(QStringLiteral("private"));
    network.firewallProfiles = DiagnosticFact::available(QStringLiteral("private=on"));
    network.executableFirewallRule = DiagnosticFact::available(QStringLiteral("confirmed"));
    return network;
}

NetworkEnvironmentFact networkFact(const QString &prefix) {
    return networkFact(QStringList{prefix});
}

class FakeNetworkMonitorOperations {
public:
    NetworkMonitorOperations asOperations() {
        return {
            [this](NetworkMonitorOperations::ChangeCallback callback, void **handle) {
                interfaceCallback = std::move(callback);
                *handle = &interfaceHandle;
                return registerInterfaceResult;
            },
            [this](NetworkMonitorOperations::ChangeCallback callback, void **handle) {
                routeCallback = std::move(callback);
                *handle = &routeHandle;
                return registerRouteResult;
            },
            [this](void *handle) {
                if (handle == &interfaceHandle) {
                    cancelInterfaceCalled = true;
                    ++cancelInterfaceCount;
                }
                if (handle == &routeHandle) {
                    cancelRouteCalled = true;
                    ++cancelRouteCount;
                }
            },
            [this](QDeadlineTimer) {
                ++recollectCalls;
                return snapshots.isEmpty()
                    ? DiagnosticValue<NetworkEnvironmentFact>::unavailable()
                    : snapshots.takeFirst();
            }
        };
    }

    void fireInterfaceChanged() { if (interfaceCallback) interfaceCallback(); }
    void fireRouteChanged() { if (routeCallback) routeCallback(); }

    QList<DiagnosticValue<NetworkEnvironmentFact>> snapshots;
    bool registerInterfaceResult = true;
    bool registerRouteResult = true;
    bool cancelInterfaceCalled = false;
    bool cancelRouteCalled = false;
    int cancelInterfaceCount = 0;
    int cancelRouteCount = 0;
    int recollectCalls = 0;

private:
    int interfaceHandle = 0;
    int routeHandle = 0;
    NetworkMonitorOperations::ChangeCallback interfaceCallback;
    NetworkMonitorOperations::ChangeCallback routeCallback;
};

void processEventsFor(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace

class NetworkDiagnosticsMonitorTest final : public QObject {
    Q_OBJECT

private slots:
    void emitsOnlyWhenPrivacyFilteredSnapshotChanges() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.snapshots = {DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.1.xxx/24")),
                         DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.1.xxx/24")),
                         DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.2.xxx/24"))};
        NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations(), 0);
        QVERIFY(monitor.start());
        ops.fireInterfaceChanged(); QCoreApplication::processEvents();
        QCOMPARE(countEvents(sink, QStringLiteral("network_changed")), 0);
        ops.fireRouteChanged(); QCoreApplication::processEvents();
        QCOMPARE(countEvents(sink, QStringLiteral("network_changed")), 1);
        monitor.stop();
        QVERIFY(ops.cancelInterfaceCalled); QVERIFY(ops.cancelRouteCalled);
    }

    void startsFromProvidedBaselineWithoutASecondStartupRecollection() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.snapshots = {DiagnosticValue<NetworkEnvironmentFact>::available(
            networkFact("192.168.2.xxx/24"))};
        const auto initial = DiagnosticValue<NetworkEnvironmentFact>::available(
            networkFact("192.168.1.xxx/24"));
        NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations(), initial, 0);
        QVERIFY(monitor.start());
        QCOMPARE(ops.recollectCalls, 0);
        ops.fireInterfaceChanged();
        QCoreApplication::processEvents();
        QCOMPARE(ops.recollectCalls, 1);
        QCOMPARE(countEvents(sink, QStringLiteral("network_changed")), 1);
    }

    void ignoresReorderedPrivacyFilteredPrefixesButRecordsActualPrefixChanges() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.snapshots = {
            DiagnosticValue<NetworkEnvironmentFact>::available(networkFact(
                {QStringLiteral("10.xxx.xxx.xxx/8"), QStringLiteral("192.168.1.xxx/24")})),
            DiagnosticValue<NetworkEnvironmentFact>::available(networkFact(
                {QStringLiteral("192.168.1.xxx/24"), QStringLiteral("10.xxx.xxx.xxx/8")})),
            DiagnosticValue<NetworkEnvironmentFact>::available(networkFact(
                {QStringLiteral("10.xxx.xxx.xxx/8"), QStringLiteral("192.168.2.xxx/24")}))
        };
        NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations(), 0);
        QVERIFY(monitor.start());
        ops.fireInterfaceChanged();
        QCoreApplication::processEvents();
        QCOMPARE(countEvents(sink, QStringLiteral("network_changed")), 0);
        ops.fireRouteChanged();
        QCoreApplication::processEvents();
        QCOMPARE(countEvents(sink, QStringLiteral("network_changed")), 1);
    }

    void cancelsHandleWrittenByFailedInterfaceRegistration() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.registerInterfaceResult = false;
        {
            NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations(), 0);
            QVERIFY(!monitor.start());
            QCOMPARE(ops.cancelInterfaceCount, 1);
            QCOMPARE(ops.cancelRouteCount, 0);
        }
        QCOMPARE(ops.cancelInterfaceCount, 1);
        QCOMPARE(ops.cancelRouteCount, 0);
    }

    void cancelsHandlesWrittenByFailedRouteRegistration() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.registerRouteResult = false;
        {
            NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations(), 0);
            QVERIFY(!monitor.start());
            QCOMPARE(ops.cancelInterfaceCount, 1);
            QCOMPARE(ops.cancelRouteCount, 1);
        }
        QCOMPARE(ops.cancelInterfaceCount, 1);
        QCOMPARE(ops.cancelRouteCount, 1);
    }

    void coalescesCallbackBurstsIntoOneDebouncedRecollection() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.snapshots = {DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.1.xxx/24")),
                         DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.2.xxx/24"))};
        NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations());
        QVERIFY(monitor.start());
        for (int index = 0; index != 10; ++index)
            ops.fireInterfaceChanged();
        processEventsFor(600);
        QCOMPARE(ops.recollectCalls, 2);
        QCOMPARE(countEvents(sink, QStringLiteral("network_changed")), 1);
    }

    void ignoresLateCallbacksAfterStop() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.snapshots = {DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.1.xxx/24")),
                         DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.2.xxx/24"))};
        NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations(), 0);
        QVERIFY(monitor.start());
        monitor.stop();
        ops.fireInterfaceChanged();
        QCoreApplication::processEvents();
        QCOMPARE(ops.recollectCalls, 1);
        QCOMPARE(sink.events.size(), 0);
    }

    void recordsTimedOutRecollectionWithoutThrowing() {
        CollectingSink sink;
        FakeNetworkMonitorOperations ops;
        ops.snapshots = {DiagnosticValue<NetworkEnvironmentFact>::available(networkFact("192.168.1.xxx/24")),
                         DiagnosticValue<NetworkEnvironmentFact>::timedOut()};
        NetworkDiagnosticsMonitor monitor(&sink, ops.asOperations(), 0);
        QVERIFY(monitor.start());
        ops.fireRouteChanged();
        QCoreApplication::processEvents();
        QCOMPARE(countEvents(sink, QStringLiteral("network_changed")), 1);
        QCOMPARE(findEvent(sink, QStringLiteral("network_changed")).fields.value(QStringLiteral("result")),
                 QStringLiteral("timed_out"));
    }
};

QTEST_GUILESS_MAIN(NetworkDiagnosticsMonitorTest)
#include "NetworkDiagnosticsMonitorTest.moc"

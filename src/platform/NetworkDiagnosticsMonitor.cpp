#include "platform/NetworkDiagnosticsMonitor.h"

#include "diagnostics/DiagnosticLogSink.h"
#include "platform/WindowsEnvironmentDiagnostics.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

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
#include <memory>

namespace {

QString statusText(DiagnosticFactStatus status) {
    switch (status) {
    case DiagnosticFactStatus::Available: return QStringLiteral("available");
    case DiagnosticFactStatus::TimedOut: return QStringLiteral("timed_out");
    case DiagnosticFactStatus::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

struct NativeNotificationContext {
    NetworkMonitorOperations::ChangeCallback callback;
    std::atomic_bool accepting{true};
};

struct NativeNotificationHandle {
    HANDLE notification = nullptr;
    std::unique_ptr<NativeNotificationContext> context;
};

void invokeNativeCallback(NativeNotificationContext *context) {
    if (context != nullptr && context->accepting.load(std::memory_order_acquire) &&
        context->callback) {
        context->callback();
    }
}

VOID CALLBACK interfaceChanged(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
    invokeNativeCallback(static_cast<NativeNotificationContext *>(context));
}

VOID CALLBACK routeChanged(PVOID context, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE) {
    invokeNativeCallback(static_cast<NativeNotificationContext *>(context));
}

bool registerNotification(NetworkMonitorOperations::ChangeCallback callback, void **output,
                          bool route) {
    if (output == nullptr)
        return false;
    auto handle = std::make_unique<NativeNotificationHandle>();
    handle->context = std::make_unique<NativeNotificationContext>();
    handle->context->callback = std::move(callback);
    const DWORD result = route
        ? NotifyRouteChange2(AF_UNSPEC, routeChanged, handle->context.get(), FALSE,
                             &handle->notification)
        : NotifyIpInterfaceChange(AF_UNSPEC, interfaceChanged, handle->context.get(), FALSE,
                                  &handle->notification);
    if (result != NO_ERROR)
        return false;
    *output = handle.release();
    return true;
}

void cancelNotification(void *opaque) {
    auto *handle = static_cast<NativeNotificationHandle *>(opaque);
    if (handle == nullptr)
        return;
    handle->context->accepting.store(false, std::memory_order_release);
    CancelMibChangeNotify2(handle->notification);
    delete handle;
}

EnvironmentSnapshot networkSnapshot(const DiagnosticValue<NetworkEnvironmentFact> &network) {
    EnvironmentSnapshot snapshot;
    snapshot.network = network;
    return snapshot;
}

} // namespace

NetworkMonitorOperations windowsNetworkMonitorOperations() {
    const EnvironmentDiagnosticProviders providers = windowsEnvironmentDiagnosticProviders();
    return {
        [](NetworkMonitorOperations::ChangeCallback callback, void **handle) {
            return registerNotification(std::move(callback), handle, false);
        },
        [](NetworkMonitorOperations::ChangeCallback callback, void **handle) {
            return registerNotification(std::move(callback), handle, true);
        },
        cancelNotification,
        providers.network
    };
}

NetworkDiagnosticsMonitor::NetworkDiagnosticsMonitor(DiagnosticLogSink *sink,
                                                     NetworkMonitorOperations operations,
                                                     int debounceMs, QObject *parent)
    : QObject(parent), sink_(sink), operations_(std::move(operations)),
      timer_(new QTimer(this)), debounceMs_(qMax(0, debounceMs)) {
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &NetworkDiagnosticsMonitor::recollect);
}

NetworkDiagnosticsMonitor::~NetworkDiagnosticsMonitor() {
    stop();
}

bool NetworkDiagnosticsMonitor::start() {
    if (accepting_.load(std::memory_order_acquire))
        return true;
    if (sink_ == nullptr || !operations_.registerInterface || !operations_.registerRoute ||
        !operations_.cancel || !operations_.recollect) {
        return false;
    }

    const DiagnosticValue<NetworkEnvironmentFact> initial = operations_.recollect(QDeadlineTimer(3000));
    lastKey_ = privacyFilteredKey(initial);
    accepting_.store(true, std::memory_order_release);

    const QPointer<NetworkDiagnosticsMonitor> guard(this);
    const NetworkMonitorOperations::ChangeCallback callback = [guard] {
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard] {
            if (guard)
                guard->requestRecollection();
        }, Qt::QueuedConnection);
    };
    if (!operations_.registerInterface(callback, &interfaceHandle_)) {
        accepting_.store(false, std::memory_order_release);
        return false;
    }
    if (!operations_.registerRoute(callback, &routeHandle_)) {
        accepting_.store(false, std::memory_order_release);
        operations_.cancel(interfaceHandle_);
        interfaceHandle_ = nullptr;
        return false;
    }
    return true;
}

void NetworkDiagnosticsMonitor::stop() {
    accepting_.store(false, std::memory_order_release);
    if (operations_.cancel) {
        if (interfaceHandle_ != nullptr)
            operations_.cancel(interfaceHandle_);
        if (routeHandle_ != nullptr)
            operations_.cancel(routeHandle_);
    }
    timer_->stop();
    interfaceHandle_ = nullptr;
    routeHandle_ = nullptr;
}

void NetworkDiagnosticsMonitor::requestRecollection() {
    if (!accepting_.load(std::memory_order_acquire))
        return;
    if (debounceMs_ == 0) {
        recollect();
        return;
    }
    timer_->start(debounceMs_);
}

void NetworkDiagnosticsMonitor::recollect() {
    if (!accepting_.load(std::memory_order_acquire))
        return;
    const DiagnosticValue<NetworkEnvironmentFact> current = operations_.recollect(QDeadlineTimer(3000));
    if (current.status == DiagnosticFactStatus::TimedOut) {
        recordChanged(current);
        return;
    }
    const QString currentKey = privacyFilteredKey(current);
    if (currentKey == lastKey_)
        return;
    lastKey_ = currentKey;
    recordChanged(current);
}

QString NetworkDiagnosticsMonitor::privacyFilteredKey(
    const DiagnosticValue<NetworkEnvironmentFact> &network) const {
    const QList<DiagnosticEvent> events = EnvironmentDiagnostics::events(networkSnapshot(network));
    if (events.isEmpty())
        return statusText(network.status);
    const DiagnosticEvent &snapshot = events.constFirst();
    QStringList parts{
        statusText(network.status),
        snapshot.fields.value(QStringLiteral("network_category")),
        snapshot.fields.value(QStringLiteral("firewall_profiles")),
        snapshot.fields.value(QStringLiteral("firewall_rule"))
    };
    QStringList adapters;
    for (qsizetype index = 1; index < events.size(); ++index) {
        const QMap<QString, QString> &fields = events.at(index).fields;
        adapters.append(fields.value(QStringLiteral("type")) + QLatin1Char('|') +
                        fields.value(QStringLiteral("up")) + QLatin1Char('|') +
                        fields.value(QStringLiteral("default_route")) + QLatin1Char('|') +
                        fields.value(QStringLiteral("route_metric")) + QLatin1Char('|') +
                        fields.value(QStringLiteral("prefixes")));
    }
    std::sort(adapters.begin(), adapters.end());
    parts.append(adapters.join(QLatin1Char(';')));
    return parts.join(QChar(0x1f));
}

void NetworkDiagnosticsMonitor::recordChanged(
    const DiagnosticValue<NetworkEnvironmentFact> &network) {
    if (!accepting_.load(std::memory_order_acquire) || sink_ == nullptr)
        return;
    const QList<DiagnosticEvent> events = EnvironmentDiagnostics::events(networkSnapshot(network));
    const DiagnosticEvent snapshot = events.isEmpty() ? DiagnosticEvent{} : events.constFirst();
    const QString category = snapshot.fields.value(QStringLiteral("network_category"),
                                                    statusText(network.status));
    const QString firewall = snapshot.fields.value(QStringLiteral("firewall_profiles"),
                                                    QStringLiteral("unavailable")) +
        QLatin1Char(':') + snapshot.fields.value(QStringLiteral("firewall_rule"),
                                                  QStringLiteral("unavailable"));
    sink_->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("environment"),
                                      QStringLiteral("network_changed"),
                                      {{QStringLiteral("adapter_count"),
                                        QString::number(network.value.adapters.size())},
                                       {QStringLiteral("category"), category},
                                       {QStringLiteral("firewall"), firewall},
                                       {QStringLiteral("result"), statusText(network.status)}}, true));
    if (network.status == DiagnosticFactStatus::TimedOut)
        return;
    for (qsizetype index = 1; index < events.size(); ++index)
        sink_->record(events.at(index));
}

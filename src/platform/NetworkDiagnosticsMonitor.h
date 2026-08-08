#pragma once

#include "platform/EnvironmentDiagnostics.h"

#include <QObject>

#include <atomic>
#include <functional>

class DiagnosticLogSink;
class QTimer;

struct NetworkMonitorOperations {
    using ChangeCallback = std::function<void()>;

    std::function<bool(ChangeCallback, void **)> registerInterface;
    std::function<bool(ChangeCallback, void **)> registerRoute;
    std::function<void(void *)> cancel;
    std::function<DiagnosticValue<NetworkEnvironmentFact>(QDeadlineTimer)> recollect;
};

NetworkMonitorOperations windowsNetworkMonitorOperations();

class NetworkDiagnosticsMonitor final : public QObject {
    Q_OBJECT

public:
    explicit NetworkDiagnosticsMonitor(DiagnosticLogSink *sink,
                                       NetworkMonitorOperations operations,
                                       int debounceMs = 500,
                                       QObject *parent = nullptr);
    ~NetworkDiagnosticsMonitor() override;

    bool start();
    void stop();

private slots:
    void requestRecollection();
    void recollect();

private:
    QString privacyFilteredKey(const DiagnosticValue<NetworkEnvironmentFact> &network) const;
    void recordChanged(const DiagnosticValue<NetworkEnvironmentFact> &network);

    DiagnosticLogSink *sink_ = nullptr;
    NetworkMonitorOperations operations_;
    QTimer *timer_ = nullptr;
    void *interfaceHandle_ = nullptr;
    void *routeHandle_ = nullptr;
    QString lastKey_;
    std::atomic_bool accepting_{false};
    int debounceMs_ = 500;
};

#pragma once

#include "app/DiagnosticActivation.h"
#include "diagnostics/DiagnosticLogSink.h"
#include "platform/EnvironmentDiagnostics.h"
#include "platform/NetworkDiagnosticsMonitor.h"

#include <functional>
#include <memory>
#include <optional>

class DiagnosticSessionStorage;

struct DiagnosticLifecycleStart {
    QString applicationDirectory;
    DiagnosticActivation activation;
    std::optional<DiagnosticEvent> beforeChildGateEvent;
    std::function<void(QString)> reportWriteFailure;
};

struct DiagnosticLifecycleStartupResult {
    DiagnosticChildGateResult childGate = DiagnosticChildGateResult::ContinueStartup;
    QString creationError;
};

struct DiagnosticLifecycleDependencies {
    std::shared_ptr<DiagnosticSessionStorage> sessionStorage;
    DiagnosticChildGateOperations childGate;
    std::optional<EnvironmentDiagnosticProviders> environmentProviders;
    std::optional<NetworkMonitorOperations> networkMonitorOperations;
};

// Construct on the application thread before consumers that borrow sink().
// start and environment collection are called once; the caller explicitly chooses
// abortStartup or exitNormally. Destruction only releases resources.
class DiagnosticLifecycle final {
public:
    explicit DiagnosticLifecycle(QObject *deliveryContext,
                                 DiagnosticLifecycleDependencies dependencies = {});
    ~DiagnosticLifecycle();

    DiagnosticLifecycleStartupResult start(DiagnosticLifecycleStart options);
    DiagnosticLogSink *sink() const;
    bool loggingActive() const;
    void collectEnvironmentAndStartMonitor();
    // Borrowed sinks must not be used after abortStartup; normal close retains them.
    void abortStartup(QString reason);
    void exitNormally(std::function<void()> stopReceiver);

private:
    class Private;
    std::unique_ptr<Private> d;
};

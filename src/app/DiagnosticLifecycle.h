#pragma once

#include "app/DiagnosticActivation.h"
#include "diagnostics/DiagnosticLogSink.h"
#include "platform/EnvironmentDiagnostics.h"
#include "platform/NetworkDiagnosticsMonitor.h"

#include <functional>
#include <memory>
#include <optional>

class DiagnosticSessionStorage;

enum class DiagnosticFailureKind { Creation, Write };

struct DiagnosticFailure {
    DiagnosticFailureKind kind;
    QString error;
};

struct DiagnosticLifecycleStart {
    QString applicationDirectory;
    DiagnosticActivation activation;
    std::optional<DiagnosticEvent> beforeChildGateEvent;
    std::function<void(DiagnosticFailure)> reportFailure;
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

// Construct and call lifecycle methods on the application thread, before consumers
// that borrow sink(). Borrowed sinks may record from worker threads.
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
    // Enable only after the window and its state/warning connections are ready.
    void enableFailureReporting();
    // Returned failures are already claimed; late queued delivery cannot replay them.
    // Borrowed sinks must not be used after abortStartup; normal close retains them.
    std::optional<DiagnosticFailure> abortStartup(QString reason);
    std::optional<DiagnosticFailure> exitNormally(std::function<void()> stopReceiver);

private:
    class Private;
    std::unique_ptr<Private> d;
};

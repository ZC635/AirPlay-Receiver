#include "app/DiagnosticLifecycle.h"

#include "diagnostics/DiagnosticSession.h"
#include "diagnostics/QtDiagnosticMessageBridge.h"
#include "platform/WindowsEnvironmentDiagnostics.h"

class DiagnosticLifecycle::Private {
public:
    Private(QObject *deliveryContext, DiagnosticLifecycleDependencies operations)
        : writeFailureContext(deliveryContext), dependencies(std::move(operations)) {}

    void stopResources() {
        if (networkMonitor) {
            networkMonitor->stop();
            networkMonitor.reset();
        }
        qtBridge.reset();
    }

    void releaseSession() {
        stopResources();
        session.reset();
        sink = &nullDiagnosticLogSink();
        finished = true;
    }

    // A lifetime-scoped context keeps delivery queued on the application's thread,
    // and discards pending calls when the owner itself is destroyed.
    QObject writeFailureContext;
    DiagnosticLifecycleDependencies dependencies;
    std::unique_ptr<DiagnosticSession> session;
    std::optional<QtDiagnosticMessageBridge> qtBridge;
    std::unique_ptr<NetworkDiagnosticsMonitor> networkMonitor;
    DiagnosticLogSink *sink = &nullDiagnosticLogSink();
    bool started = false;
    bool finished = false;
    bool environmentCollected = false;
};

DiagnosticLifecycle::DiagnosticLifecycle(QObject *deliveryContext,
                                       DiagnosticLifecycleDependencies dependencies)
    : d(std::make_unique<Private>(deliveryContext, std::move(dependencies))) {}

DiagnosticLifecycle::~DiagnosticLifecycle() {
    d->stopResources();
}

DiagnosticLifecycleStartupResult DiagnosticLifecycle::start(DiagnosticLifecycleStart options) {
    Q_ASSERT(!d->started);
    d->started = true;
    DiagnosticLifecycleStartupResult result;
    if (options.activation.enabled) {
        DiagnosticSessionOptions sessionOptions;
        sessionOptions.applicationDirectory = std::move(options.applicationDirectory);
        sessionOptions.activationSource = options.activation.sourceName();
        sessionOptions.storage = d->dependencies.sessionStorage;
        auto created = DiagnosticSession::create(std::move(sessionOptions));
        result.creationError = std::move(created.error);
        d->session = std::move(created.session);
        if (d->session) {
            d->sink = d->session.get();
            QObject::connect(d->session.get(), &DiagnosticSession::writeFailed,
                &d->writeFailureContext,
                [handler = std::move(options.reportWriteFailure)](const QString &error) {
                    if (handler)
                        handler(error);
                }, Qt::QueuedConnection);
            d->qtBridge.emplace(d->sink);
        }
    }
    if (options.beforeChildGateEvent)
        d->sink->record(std::move(*options.beforeChildGateEvent));
    result.childGate = runDiagnosticChildGate(options.activation, d->session.get(),
        result.creationError, d->dependencies.childGate);
    if (result.childGate == DiagnosticChildGateResult::ExitChild)
        d->releaseSession();
    return result;
}

DiagnosticLogSink *DiagnosticLifecycle::sink() const {
    return d->sink;
}

bool DiagnosticLifecycle::loggingActive() const {
    return d->session && d->session->isActive();
}

void DiagnosticLifecycle::collectEnvironmentAndStartMonitor() {
    if (d->finished || d->environmentCollected || !loggingActive())
        return;
    d->environmentCollected = true;
    const EnvironmentDiagnosticProviders providers = d->dependencies.environmentProviders
        ? *d->dependencies.environmentProviders : windowsEnvironmentDiagnosticProviders();
    const EnvironmentSnapshot snapshot = EnvironmentDiagnostics::collect(providers, 3000);
    for (const DiagnosticEvent &event : EnvironmentDiagnostics::events(snapshot))
        d->sink->record(event);
    if (!loggingActive())
        return;
    NetworkMonitorOperations operations = d->dependencies.networkMonitorOperations
        ? *d->dependencies.networkMonitorOperations : windowsNetworkMonitorOperations();
    d->networkMonitor = std::make_unique<NetworkDiagnosticsMonitor>(
        d->sink, std::move(operations), snapshot.network);
    if (!d->networkMonitor->start()) {
        d->sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("startup"),
            QStringLiteral("network_monitor"), {{QStringLiteral("result"), QStringLiteral("unavailable")}}, true));
        d->networkMonitor.reset();
    }
}

void DiagnosticLifecycle::abortStartup(QString reason) {
    if (d->finished)
        return;
    d->sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("startup"),
        QStringLiteral("startup_aborted"), {{QStringLiteral("reason"), std::move(reason)}}, true));
    d->releaseSession();
}

void DiagnosticLifecycle::exitNormally(std::function<void()> stopReceiver) {
    if (d->finished)
        return;
    d->sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("startup"),
        QStringLiteral("shutdown_started"), {}, true));
    if (stopReceiver)
        stopReceiver();
    d->stopResources();
    if (d->session)
        d->session->closeNormally();
    // Consumers (receiver/window/language) still borrow the closed sink until they
    // are destroyed. Releasing the session here would leave them dangling.
    d->finished = true;
}

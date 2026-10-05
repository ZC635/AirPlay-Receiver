#include "app/DiagnosticLifecycle.h"

#include "diagnostics/DiagnosticSession.h"
#include "diagnostics/QtDiagnosticMessageBridge.h"
#include "platform/WindowsEnvironmentDiagnostics.h"

#include <QCoreApplication>
#include <QThread>

class DiagnosticLifecycle::Private {
public:
    enum class Phase { AwaitingUi, Reporting, Closing, Finished };

    Private(QObject *deliveryContext, DiagnosticLifecycleDependencies operations)
        : writeFailureContext(deliveryContext), dependencies(std::move(operations)) {
        assertApplicationThread();
    }

    void assertApplicationThread() const {
        Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
        Q_ASSERT(writeFailureContext.thread() == QThread::currentThread());
    }

    std::optional<DiagnosticFailure> claimFailure() {
        assertApplicationThread();
        if (delivered)
            return std::nullopt;
        std::optional<DiagnosticFailure> failure;
        if (!creationError.isEmpty())
            failure = DiagnosticFailure{DiagnosticFailureKind::Creation, creationError};
        else if (session) {
            const QString error = session->writeFailure();
            if (!error.isEmpty())
                failure = DiagnosticFailure{DiagnosticFailureKind::Write, error};
        }
        if (failure)
            delivered = true;
        return failure;
    }

    void reportPendingFailure() {
        assertApplicationThread();
        if (phase != Phase::Reporting || !reportFailure)
            return;
        const auto failure = claimFailure();
        if (failure) {
            // Claim before a handler can enter a modal loop or terminate the owner.
            const auto handler = reportFailure;
            handler(*failure);
        }
    }

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
    }

    // A lifetime-scoped context keeps delivery queued on the application's thread,
    // and discards pending calls when the owner itself is destroyed.
    QObject writeFailureContext;
    DiagnosticLifecycleDependencies dependencies;
    std::unique_ptr<DiagnosticSession> session;
    std::optional<QtDiagnosticMessageBridge> qtBridge;
    std::unique_ptr<NetworkDiagnosticsMonitor> networkMonitor;
    DiagnosticLogSink *sink = &nullDiagnosticLogSink();
    std::function<void(DiagnosticFailure)> reportFailure;
    QString creationError;
    Phase phase = Phase::AwaitingUi;
    bool delivered = false;
    bool started = false;
    bool environmentCollected = false;
};

DiagnosticLifecycle::DiagnosticLifecycle(QObject *deliveryContext,
                                       DiagnosticLifecycleDependencies dependencies)
    : d(std::make_unique<Private>(deliveryContext, std::move(dependencies))) {}

DiagnosticLifecycle::~DiagnosticLifecycle() {
    d->assertApplicationThread();
    d->phase = Private::Phase::Finished;
    d->stopResources();
}

DiagnosticLifecycleStartupResult DiagnosticLifecycle::start(DiagnosticLifecycleStart options) {
    d->assertApplicationThread();
    Q_ASSERT(!d->started);
    d->started = true;
    d->reportFailure = std::move(options.reportFailure);
    DiagnosticLifecycleStartupResult result;
    if (options.activation.enabled) {
        DiagnosticSessionOptions sessionOptions;
        sessionOptions.applicationDirectory = std::move(options.applicationDirectory);
        sessionOptions.activationSource = options.activation.sourceName();
        sessionOptions.storage = d->dependencies.sessionStorage;
        auto created = DiagnosticSession::create(std::move(sessionOptions));
        result.creationError = std::move(created.error);
        d->creationError = result.creationError;
        d->session = std::move(created.session);
        if (d->session) {
            d->sink = d->session.get();
            QObject::connect(d->session.get(), &DiagnosticSession::writeFailed,
                &d->writeFailureContext,
                [state = d.get()] { state->reportPendingFailure(); }, Qt::QueuedConnection);
            d->qtBridge.emplace(d->sink);
        }
    }
    if (options.beforeChildGateEvent)
        d->sink->record(std::move(*options.beforeChildGateEvent));
    result.childGate = runDiagnosticChildGate(options.activation, d->session.get(),
        result.creationError, d->dependencies.childGate);
    if (result.childGate == DiagnosticChildGateResult::ExitChild) {
        d->phase = Private::Phase::Finished;
        d->releaseSession();
    }
    return result;
}

DiagnosticLogSink *DiagnosticLifecycle::sink() const {
    return d->sink;
}

bool DiagnosticLifecycle::loggingActive() const {
    return d->session && d->session->isActive();
}

void DiagnosticLifecycle::collectEnvironmentAndStartMonitor() {
    d->assertApplicationThread();
    if (d->phase == Private::Phase::Closing || d->phase == Private::Phase::Finished ||
        d->environmentCollected || !loggingActive())
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

void DiagnosticLifecycle::enableFailureReporting() {
    d->assertApplicationThread();
    if (d->phase == Private::Phase::AwaitingUi)
        d->phase = Private::Phase::Reporting;
    d->reportPendingFailure();
}

std::optional<DiagnosticFailure> DiagnosticLifecycle::abortStartup(QString reason) {
    d->assertApplicationThread();
    if (d->phase == Private::Phase::Closing || d->phase == Private::Phase::Finished)
        return std::nullopt;
    d->phase = Private::Phase::Closing;
    d->sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("startup"),
        QStringLiteral("startup_aborted"), {{QStringLiteral("reason"), std::move(reason)}}, true));
    d->stopResources();
    const auto failure = d->claimFailure();
    d->releaseSession();
    d->phase = Private::Phase::Finished;
    return failure;
}

std::optional<DiagnosticFailure> DiagnosticLifecycle::exitNormally(std::function<void()> stopReceiver) {
    d->assertApplicationThread();
    if (d->phase == Private::Phase::Closing || d->phase == Private::Phase::Finished)
        return std::nullopt;
    d->phase = Private::Phase::Closing;
    d->sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("startup"),
        QStringLiteral("shutdown_started"), {}, true));
    if (stopReceiver)
        stopReceiver();
    d->stopResources();
    if (d->session)
        d->session->closeNormally();
    // Consumers (receiver/window/language) still borrow the closed sink until they
    // are destroyed. Releasing the session here would leave them dangling.
    d->phase = Private::Phase::Finished;
    return d->claimFailure();
}

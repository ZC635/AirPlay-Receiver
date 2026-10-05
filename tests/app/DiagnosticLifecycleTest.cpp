#include "app/DiagnosticLifecycle.h"

#include "diagnostics/DiagnosticSession.h"
#include "support/MemoryDiagnosticStorage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <thread>

namespace {

using DiagnosticTestSupport::MemoryStorage;

DiagnosticLifecycleStart enabledStart(const QString &directory) {
    DiagnosticLifecycleStart start;
    start.applicationDirectory = directory;
    start.activation = {true, DiagnosticActivationSource::CommandArgument};
    return start;
}

QByteArray logBytes(const QString &directory) {
    QDir logs(directory + QStringLiteral("/logs"));
    QByteArray bytes;
    for (const QString &name : logs.entryList({QStringLiteral("AirPlay-Diagnostic-*.log")}, QDir::Files)) {
        QFile file(logs.filePath(name));
        if (file.open(QIODevice::ReadOnly))
            bytes += file.readAll();
    }
    return bytes;
}

struct NetworkOperations {
    int interfaceHandle = 0;
    int routeHandle = 0;
    int registrations = 0;
    int cancellations = 0;
    int recollections = 0;
    bool routeSucceeds = true;
    NetworkMonitorOperations::ChangeCallback interfaceChanged;
    std::function<void()> onCancel;

    NetworkMonitorOperations operations() {
        return {
            [this](auto callback, void **handle) {
                ++registrations;
                interfaceChanged = std::move(callback);
                *handle = &interfaceHandle;
                return true;
            },
            [this](auto, void **handle) {
                ++registrations;
                *handle = &routeHandle;
                return routeSucceeds;
            },
            [this](void *) {
                ++cancellations;
                if (onCancel)
                    onCancel();
            },
            [this](QDeadlineTimer) {
                ++recollections;
                return DiagnosticValue<NetworkEnvironmentFact>::unavailable();
            }
        };
    }
};

DiagnosticLifecycleDependencies memoryDependencies(const std::shared_ptr<MemoryStorage> &storage) {
    DiagnosticLifecycleDependencies dependencies;
    dependencies.sessionStorage = storage;
    return dependencies;
}

DiagnosticLifecycleDependencies environmentDependencies(
    NetworkOperations &network, int &collections, qint64 &deadlineMs) {
    DiagnosticLifecycleDependencies dependencies;
    EnvironmentDiagnosticProviders providers;
    providers.operatingSystem = [&deadlineMs](QDeadlineTimer deadline) {
        deadlineMs = deadline.remainingTime();
        return DiagnosticFact::available(QStringLiteral("Windows 11"));
    };
    providers.network = [&collections](QDeadlineTimer) {
        ++collections;
        return DiagnosticValue<NetworkEnvironmentFact>::available({});
    };
    dependencies.environmentProviders = std::move(providers);
    dependencies.networkMonitorOperations = network.operations();
    return dependencies;
}

void ignoreQtMessage(QtMsgType, const QMessageLogContext &, const QString &) {}

class ScopedMessageHandler final {
public:
    ScopedMessageHandler() : previous_(qInstallMessageHandler(ignoreQtMessage)) {}
    ~ScopedMessageHandler() { qInstallMessageHandler(previous_); }
private:
    QtMessageHandler previous_;
};

} // namespace

class DiagnosticLifecycleTest final : public QObject {
    Q_OBJECT
private slots:
    void normalLaunchCreatesNoLogButStillStopsReceiver();
    void enabledLaunchCreatesActiveSession();
    void creationFailureReturnsErrorAndNullSink();
    void childReadyFollowsTranslationEventAndParentExit();
    void rejectedChildReleasesResourcesWithoutNormalMarker_data();
    void rejectedChildReleasesResourcesWithoutNormalMarker();
    void earlyWriteFailureIsQueuedAndDeliveredOnce();
    void earlyFailureSurvivesEventProcessing();
    void creationFailureIsReportedWhenUiReady();
    void missingHandlerKeepsFailureForTerminalResult_data();
    void missingHandlerKeepsFailureForTerminalResult();
    void handlerReentryDoesNotReportTwice();
    void stopReceiverEventsDoNotReportBeforeReturn();
    void terminalReentryDoesNotRepeatMarkers();
    void abortBoundaryFailureReturned_data();
    void abortBoundaryFailureReturned();
    void destroyedOwnerDropsPendingDelivery();
    void environmentBaselineAvoidsSecondStartupCollection();
    void inactiveSessionSkipsEnvironmentAndMonitor_data();
    void inactiveSessionSkipsEnvironmentAndMonitor();
    void monitorRegistrationFailureIsRecordedAndCleanedUp();
    void normalExitKeepsBridgeAndMonitorThroughReceiverStop();
    void startupAbortRecordsReasonWithoutNormalMarker();
    void destructorOnlyCleansUp();
    void terminalBoundaryFailureReturned_data();
    void terminalBoundaryFailureReturned();
};

void DiagnosticLifecycleTest::normalLaunchCreatesNoLogButStillStopsReceiver() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    int cpuCollections = 0;
    DiagnosticLifecycleDependencies dependencies;
    EnvironmentDiagnosticProviders providers;
    providers.cpuCapabilities = [&cpuCollections](QDeadlineTimer) {
        ++cpuCollections;
        return DiagnosticValue<CpuEnvironmentFact>::available({});
    };
    dependencies.environmentProviders = std::move(providers);
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), std::move(dependencies));
    DiagnosticLifecycleStart start;
    start.applicationDirectory = directory.path();
    const auto result = diagnostics.start(std::move(start));
    QCOMPARE(result.childGate, DiagnosticChildGateResult::ContinueStartup);
    QVERIFY(result.creationError.isEmpty());
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(diagnostics.sink(), &nullDiagnosticLogSink());
    diagnostics.collectEnvironmentAndStartMonitor();
    QCOMPARE(cpuCollections, 0);
    int stops = 0;
    diagnostics.exitNormally([&stops] { ++stops; });
    QCOMPARE(stops, 1);
    QVERIFY(!QDir(directory.filePath("logs")).exists());
}

void DiagnosticLifecycleTest::enabledLaunchCreatesActiveSession() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticLifecycle diagnostics(QCoreApplication::instance());
    const auto result = diagnostics.start(enabledStart(directory.path()));
    QCOMPARE(result.childGate, DiagnosticChildGateResult::ContinueStartup);
    QVERIFY(result.creationError.isEmpty());
    QVERIFY(diagnostics.loggingActive());
    QVERIFY(diagnostics.sink()->isActive());
    DiagnosticLogSink *borrowed = diagnostics.sink();
    diagnostics.exitNormally({});
    const QByteArray bytes = logBytes(directory.path());
    QVERIFY(bytes.contains("activation_source=command_argument"));
    QVERIFY(bytes.contains("normal_exit=yes"));
    QCOMPARE(diagnostics.sink(), borrowed);
    QVERIFY(!borrowed->isActive());
    borrowed->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "after_close"));
    QCOMPARE(logBytes(directory.path()), bytes);
}

void DiagnosticLifecycleTest::creationFailureReturnsErrorAndNullSink() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile blocked(directory.filePath("logs"));
    QVERIFY(blocked.open(QIODevice::WriteOnly));
    blocked.close();
    DiagnosticLifecycle diagnostics(QCoreApplication::instance());
    const auto result = diagnostics.start(enabledStart(directory.path()));
    QCOMPARE(result.childGate, DiagnosticChildGateResult::ContinueStartup);
    QVERIFY(!result.creationError.isEmpty());
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(diagnostics.sink(), &nullDiagnosticLogSink());
    diagnostics.abortStartup(QStringLiteral("missing_runtime"));
}

void DiagnosticLifecycleTest::childReadyFollowsTranslationEventAndParentExit() {
    auto storage = std::make_shared<MemoryStorage>();
    auto dependencies = memoryDependencies(storage);
    QStringList calls;
    bool translationRecorded = false;
    QByteArray response;
    int parent = 0;
    dependencies.childGate = {
        [&](qint64 pid, QString *) -> void * { calls << QString::number(pid); return &parent; },
        [&](const QString &, const QByteArray &line, QString *) {
            calls << "ready";
            response = line;
            translationRecorded = storage->file->bytes.contains("translation_load_failed");
            return true;
        },
        [&](void *, int timeout, QString *) { calls << QString::number(timeout); return true; },
        [&](void *) { calls << "closed"; }
    };
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), std::move(dependencies));
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.activation.parentPid = 42;
    start.activation.readyToken = QStringLiteral("private-token");
    start.beforeChildGateEvent = makeDiagnosticEvent(
        DiagnosticSeverity::Warning, "startup", "translation_load_failed", {}, true);
    const auto result = diagnostics.start(std::move(start));
    QCOMPARE(result.childGate, DiagnosticChildGateResult::ContinueStartup);
    QCOMPARE(response, QByteArray("READY\n"));
    QCOMPARE(calls, QStringList({"42", "ready", "30000", "closed"}));
    QVERIFY(translationRecorded);
    QVERIFY(!storage->file->bytes.contains("private-token"));
    QVERIFY(diagnostics.loggingActive());
}

void DiagnosticLifecycleTest::rejectedChildReleasesResourcesWithoutNormalMarker_data() {
    QTest::addColumn<QString>("failure");
    QTest::newRow("creation") << QString("creation");
    QTest::newRow("inactive-before-ready") << QString("inactive");
    QTest::newRow("parent") << QString("parent");
    QTest::newRow("ready") << QString("ready");
    QTest::newRow("wait") << QString("wait");
}

void DiagnosticLifecycleTest::rejectedChildReleasesResourcesWithoutNormalMarker() {
    QFETCH(QString, failure);
    ScopedMessageHandler handler;
    auto storage = std::make_shared<MemoryStorage>();
    if (failure == "creation")
        storage->file->ensureError = "directory unavailable";
    auto dependencies = memoryDependencies(storage);
    QByteArray response;
    int closes = 0;
    int parent = 0;
    dependencies.childGate = {
        [&](qint64, QString *) -> void * { return failure == "parent" ? nullptr : &parent; },
        [&](const QString &, const QByteArray &line, QString *) {
            response = line;
            return failure != "ready";
        },
        [&](void *, int, QString *) { return failure != "wait"; },
        [&](void *) { ++closes; }
    };
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), std::move(dependencies));
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.activation.parentPid = 42;
    start.activation.readyToken = QStringLiteral("private-token");
    if (failure == "inactive") {
        start.beforeChildGateEvent = makeDiagnosticEvent(
            DiagnosticSeverity::Info, "startup", "before_ready", {}, true);
        storage->file->failWriteAt = 2; // Header succeeds; the pre-gate event deactivates it.
    }
    const auto result = diagnostics.start(std::move(start));
    QCOMPARE(result.childGate, DiagnosticChildGateResult::ExitChild);
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(diagnostics.sink(), &nullDiagnosticLogSink());
    QCOMPARE(closes, failure == "ready" || failure == "wait" ? 1 : 0);
    QVERIFY(response.startsWith(failure == "ready" || failure == "wait" ? "READY\n" : "ERROR\t"));
    qWarning("after_rejected_child");
    QVERIFY(!storage->file->bytes.contains("after_rejected_child"));
    QVERIFY(!storage->file->bytes.contains("startup_aborted"));
    QVERIFY(!storage->file->bytes.contains("normal_exit=yes"));
}

void DiagnosticLifecycleTest::earlyWriteFailureIsQueuedAndDeliveredOnce() {
    auto storage = std::make_shared<MemoryStorage>();
    int handled = 0;
    QString error;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportFailure = [&](DiagnosticFailure failure) {
        ++handled;
        QCOMPARE(failure.kind, DiagnosticFailureKind::Write);
        QCOMPARE(QThread::currentThread(), QCoreApplication::instance()->thread());
        error = std::move(failure.error);
    };
    diagnostics.start(std::move(start));
    diagnostics.enableFailureReporting();
    storage->file->failWrites = true;
    std::thread writer([&] {
        diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "worker_failure"));
    });
    writer.join();
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(handled, 0);
    QTRY_COMPARE(handled, 1);
    QCOMPARE(error, QStringLiteral("disk full"));
    diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "another_failure"));
    QVERIFY(!diagnostics.exitNormally({}));
    QCoreApplication::processEvents();
    QCOMPARE(handled, 1);
}

void DiagnosticLifecycleTest::earlyFailureSurvivesEventProcessing() {
    auto storage = std::make_shared<MemoryStorage>();
    bool attached = false;
    int callbacks = 0;
    int warnings = 0;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportFailure = [&](DiagnosticFailure) { ++callbacks; if (attached) ++warnings; };
    diagnostics.start(std::move(start));
    storage->file->failWrites = true;
    diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "before_window"));
    QCOMPARE(callbacks, 0);
    QCoreApplication::processEvents();
    QCOMPARE(callbacks, 0);
    attached = true;
    diagnostics.enableFailureReporting();
    QCOMPARE(callbacks, 1);
    diagnostics.enableFailureReporting();
    QCoreApplication::processEvents();
    QCOMPARE(callbacks, 1);
    QCOMPARE(warnings, 1);
    QVERIFY(!diagnostics.exitNormally({}));
}

void DiagnosticLifecycleTest::creationFailureIsReportedWhenUiReady() {
    auto storage = std::make_shared<MemoryStorage>();
    storage->file->ensureError = "directory unavailable";
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    int handled = 0;
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportFailure = [&](DiagnosticFailure failure) {
        ++handled;
        QCOMPARE(failure.kind, DiagnosticFailureKind::Creation);
        QCOMPARE(failure.error, QStringLiteral("directory unavailable"));
        QCOMPARE(QThread::currentThread(), QCoreApplication::instance()->thread());
    };
    const auto result = diagnostics.start(std::move(start));
    QCOMPARE(result.creationError, QStringLiteral("directory unavailable"));
    QCoreApplication::processEvents();
    QCOMPARE(handled, 0);
    diagnostics.enableFailureReporting();
    QCOMPARE(handled, 1);
    diagnostics.enableFailureReporting();
    QVERIFY(!diagnostics.exitNormally({}));
    QCoreApplication::processEvents();
    QCOMPARE(handled, 1);
}

void DiagnosticLifecycleTest::missingHandlerKeepsFailureForTerminalResult_data() {
    QTest::addColumn<bool>("creation");
    QTest::newRow("creation") << true;
    QTest::newRow("write") << false;
}

void DiagnosticLifecycleTest::missingHandlerKeepsFailureForTerminalResult() {
    QFETCH(bool, creation);
    auto storage = std::make_shared<MemoryStorage>();
    if (creation)
        storage->file->ensureError = "directory unavailable";
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    diagnostics.start(enabledStart(QStringLiteral("C:/test")));
    diagnostics.enableFailureReporting();
    if (!creation) {
        storage->file->failWrites = true;
        diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "failure"));
    }
    QCoreApplication::processEvents();
    const auto failure = diagnostics.exitNormally({});
    QVERIFY(failure);
    QCOMPARE(failure->kind, creation ? DiagnosticFailureKind::Creation : DiagnosticFailureKind::Write);
    QCOMPARE(failure->error, creation ? QStringLiteral("directory unavailable") : QStringLiteral("disk full"));
    QVERIFY(!diagnostics.exitNormally({}));
    QVERIFY(!diagnostics.abortStartup(QStringLiteral("late_abort")));
}

void DiagnosticLifecycleTest::handlerReentryDoesNotReportTwice() {
    auto storage = std::make_shared<MemoryStorage>();
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    int handled = 0;
    int stops = 0;
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportFailure = [&](DiagnosticFailure) {
        ++handled;
        diagnostics.enableFailureReporting();
        QCoreApplication::processEvents();
        QVERIFY(!diagnostics.exitNormally([&] { ++stops; }));
    };
    diagnostics.start(std::move(start));
    storage->file->failWrites = true;
    diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "failure"));
    diagnostics.enableFailureReporting();
    QCOMPARE(handled, 1);
    QCOMPARE(stops, 1);
    diagnostics.enableFailureReporting();
    QCoreApplication::processEvents();
    QCOMPARE(handled, 1);
}

void DiagnosticLifecycleTest::stopReceiverEventsDoNotReportBeforeReturn() {
    auto storage = std::make_shared<MemoryStorage>();
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    int handled = 0;
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportFailure = [&](DiagnosticFailure) { ++handled; };
    diagnostics.start(std::move(start));
    diagnostics.enableFailureReporting();
    // The queued signal predates Closing, but must remain available for the return value.
    storage->file->failWrites = true;
    diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "failure"));
    const auto failure = diagnostics.exitNormally([&] {
        diagnostics.enableFailureReporting();
        QCoreApplication::processEvents();
        QCOMPARE(handled, 0);
    });
    QVERIFY(failure);
    QCOMPARE(failure->error, QStringLiteral("disk full"));
    QCoreApplication::processEvents();
    QCOMPARE(handled, 0);
}

void DiagnosticLifecycleTest::terminalReentryDoesNotRepeatMarkers() {
    auto storage = std::make_shared<MemoryStorage>();
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    diagnostics.start(enabledStart(QStringLiteral("C:/test")));
    int stops = 0;
    QVERIFY(!diagnostics.exitNormally([&] {
        ++stops;
        QVERIFY(!diagnostics.exitNormally([&] { ++stops; }));
        QVERIFY(!diagnostics.abortStartup(QStringLiteral("reentrant")));
        diagnostics.enableFailureReporting();
        QCoreApplication::processEvents();
    }));
    QVERIFY(!diagnostics.exitNormally([&] { ++stops; }));
    QCOMPARE(stops, 1);
    QCOMPARE(storage->file->bytes.count("shutdown_started"), 1);
    QCOMPARE(storage->file->bytes.count("session_summary"), 1);
    QVERIFY(!storage->file->bytes.contains("startup_aborted"));
}

void DiagnosticLifecycleTest::abortBoundaryFailureReturned_data() {
    QTest::addColumn<QString>("boundary");
    QTest::newRow("creation") << QString("creation");
    QTest::newRow("early-write") << QString("early-write");
    QTest::newRow("abort-write") << QString("write");
    QTest::newRow("abort-flush") << QString("flush");
}

void DiagnosticLifecycleTest::abortBoundaryFailureReturned() {
    QFETCH(QString, boundary);
    auto storage = std::make_shared<MemoryStorage>();
    if (boundary == "creation")
        storage->file->ensureError = "directory unavailable";
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    int handled = 0;
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportFailure = [&](DiagnosticFailure) { ++handled; };
    diagnostics.start(std::move(start));
    storage->file->failWrites = boundary == "write" || boundary == "early-write";
    storage->file->failFlush = boundary == "flush";
    if (boundary == "early-write") {
        diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "failure"));
        QCoreApplication::processEvents();
        QCOMPARE(handled, 0);
    }
    const auto failure = diagnostics.abortStartup(QStringLiteral("missing_runtime"));
    QVERIFY(failure);
    QCOMPARE(failure->kind, boundary == "creation" ? DiagnosticFailureKind::Creation : DiagnosticFailureKind::Write);
    QCOMPARE(failure->error, boundary == "creation" ? QStringLiteral("directory unavailable") : QStringLiteral("disk full"));
    QCOMPARE(diagnostics.sink(), &nullDiagnosticLogSink());
    QVERIFY(!storage->file->bytes.contains("normal_exit=yes"));
    if (boundary == "flush")
        QVERIFY(storage->file->bytes.contains("startup_aborted reason=missing_runtime"));
    diagnostics.enableFailureReporting();
    QCoreApplication::processEvents();
    QCOMPARE(handled, 0);
    QVERIFY(!diagnostics.abortStartup(QStringLiteral("again")));
    QVERIFY(!diagnostics.exitNormally({}));
}

void DiagnosticLifecycleTest::destroyedOwnerDropsPendingDelivery() {
    auto storage = std::make_shared<MemoryStorage>();
    int handled = 0;
    {
        DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
        auto start = enabledStart(QStringLiteral("C:/test"));
        start.reportFailure = [&](DiagnosticFailure) { ++handled; };
        diagnostics.start(std::move(start));
        diagnostics.enableFailureReporting();
        storage->file->failWrites = true;
        diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "failure"));
    }
    QCoreApplication::processEvents();
    QCOMPARE(handled, 0);
    QVERIFY(!storage->file->bytes.contains("normal_exit=yes"));
}

void DiagnosticLifecycleTest::environmentBaselineAvoidsSecondStartupCollection() {
    QTemporaryDir directory;
    NetworkOperations network;
    int collections = 0, cpuCollections = 0;
    qint64 deadline = -1;
    auto dependencies = environmentDependencies(network, collections, deadline);
    dependencies.environmentProviders->cpuCapabilities = [&cpuCollections](QDeadlineTimer) {
        ++cpuCollections;
        CpuEnvironmentFact cpu;
        cpu.vendor = DiagnosticFact::available(QStringLiteral("intel"));
        cpu.hardwareFeatures = {QStringLiteral("sse2")};
        cpu.usableFeatures = cpu.hardwareFeatures;
        cpu.avxOsState = DiagnosticFact::available(QStringLiteral("disabled"));
        return DiagnosticValue<CpuEnvironmentFact>::available(cpu);
    };
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), std::move(dependencies));
    diagnostics.start(enabledStart(directory.path()));
    QCOMPARE(collections, 0);
    QCOMPARE(cpuCollections, 0);
    QCOMPARE(network.registrations, 0);
    diagnostics.collectEnvironmentAndStartMonitor();
    diagnostics.collectEnvironmentAndStartMonitor();
    QCOMPARE(collections, 1);
    QCOMPARE(cpuCollections, 1);
    QVERIFY(deadline > 0 && deadline <= 3000);
    QCOMPARE(network.registrations, 2);
    QCOMPARE(network.recollections, 0);
    const auto bytes = logBytes(directory.path());
    QVERIFY(bytes.contains("environment_snapshot"));
    QCOMPARE(bytes.count("cpu_compatibility"), 1);
    diagnostics.exitNormally({});
    QCOMPARE(network.cancellations, 2);
}

void DiagnosticLifecycleTest::inactiveSessionSkipsEnvironmentAndMonitor_data() {
    QTest::addColumn<bool>("failDuringCollection");
    QTest::newRow("inactive-before-collection") << false;
    QTest::newRow("failure-recording-snapshot") << true;
}

void DiagnosticLifecycleTest::inactiveSessionSkipsEnvironmentAndMonitor() {
    QFETCH(bool, failDuringCollection);
    auto storage = std::make_shared<MemoryStorage>();
    NetworkOperations network;
    int collections = 0;
    qint64 deadline = -1;
    auto dependencies = environmentDependencies(network, collections, deadline);
    dependencies.sessionStorage = storage;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), std::move(dependencies));
    diagnostics.start(enabledStart(QStringLiteral("C:/test")));
    storage->file->failWrites = true;
    if (!failDuringCollection)
        diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "fail"));
    diagnostics.collectEnvironmentAndStartMonitor();
    QCOMPARE(collections, failDuringCollection ? 1 : 0);
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(network.registrations, 0);
}

void DiagnosticLifecycleTest::monitorRegistrationFailureIsRecordedAndCleanedUp() {
    QTemporaryDir directory;
    NetworkOperations network;
    network.routeSucceeds = false;
    int collections = 0;
    qint64 deadline = -1;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(),
        environmentDependencies(network, collections, deadline));
    diagnostics.start(enabledStart(directory.path()));
    diagnostics.collectEnvironmentAndStartMonitor();
    QCOMPARE(network.cancellations, 2);
    QVERIFY(logBytes(directory.path()).contains("network_monitor result=unavailable"));
    diagnostics.exitNormally({});
    QCOMPARE(network.cancellations, 2);
}

void DiagnosticLifecycleTest::normalExitKeepsBridgeAndMonitorThroughReceiverStop() {
    ScopedMessageHandler handler;
    QTemporaryDir directory;
    NetworkOperations network;
    network.onCancel = [] { qWarning("monitor_cancelled"); };
    int collections = 0;
    qint64 deadline = -1;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(),
        environmentDependencies(network, collections, deadline));
    diagnostics.start(enabledStart(directory.path()));
    diagnostics.collectEnvironmentAndStartMonitor();
    bool activeWhileStopping = false;
    int cancellationsWhileStopping = -1;
    diagnostics.exitNormally([&] {
        activeWhileStopping = diagnostics.loggingActive();
        cancellationsWhileStopping = network.cancellations;
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [] { qWarning("receiver_stopping"); }, Qt::QueuedConnection);
        QCoreApplication::processEvents();
    });
    QVERIFY(activeWhileStopping);
    QCOMPARE(cancellationsWhileStopping, 0);
    QCOMPARE(network.cancellations, 2);
    const QByteArray bytes = logBytes(directory.path());
    const qsizetype shutdown = bytes.indexOf("shutdown_started");
    const qsizetype stopping = bytes.indexOf("receiver_stopping");
    const qsizetype cancelled = bytes.indexOf("monitor_cancelled");
    const qsizetype summary = bytes.indexOf("session_summary");
    QVERIFY(shutdown >= 0 && stopping > shutdown && cancelled > stopping && summary > cancelled);
    QVERIFY(bytes.contains("normal_exit=yes"));
    qWarning("after_normal_close");
    QCOMPARE(logBytes(directory.path()), bytes);
}

void DiagnosticLifecycleTest::startupAbortRecordsReasonWithoutNormalMarker() {
    QTemporaryDir directory;
    NetworkOperations network;
    int collections = 0;
    qint64 deadline = -1;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(),
        environmentDependencies(network, collections, deadline));
    diagnostics.start(enabledStart(directory.path()));
    diagnostics.collectEnvironmentAndStartMonitor();
    diagnostics.abortStartup(QStringLiteral("missing_runtime"));
    QCOMPARE(network.cancellations, 2);
    const QByteArray bytes = logBytes(directory.path());
    QVERIFY(bytes.contains("startup_aborted reason=missing_runtime"));
    QVERIFY(!bytes.contains("shutdown_started"));
    QVERIFY(!bytes.contains("normal_exit=yes"));
}

void DiagnosticLifecycleTest::destructorOnlyCleansUp() {
    QTemporaryDir directory;
    NetworkOperations network;
    int collections = 0;
    qint64 deadline = -1;
    {
        DiagnosticLifecycle diagnostics(QCoreApplication::instance(),
            environmentDependencies(network, collections, deadline));
        diagnostics.start(enabledStart(directory.path()));
        diagnostics.collectEnvironmentAndStartMonitor();
    }
    QCOMPARE(network.cancellations, 2);
    const QByteArray bytes = logBytes(directory.path());
    QVERIFY(bytes.contains("environment_snapshot"));
    QVERIFY(!bytes.contains("shutdown_started"));
    QVERIFY(!bytes.contains("startup_aborted"));
    QVERIFY(!bytes.contains("normal_exit=yes"));
    QVERIFY(bool(network.interfaceChanged));
    network.interfaceChanged();
    QCoreApplication::processEvents();
    QCOMPARE(logBytes(directory.path()), bytes);
}

void DiagnosticLifecycleTest::terminalBoundaryFailureReturned_data() {
    QTest::addColumn<QString>("boundary");
    QTest::addColumn<bool>("flushFailure");
    QTest::newRow("shutdown-write") << QString("shutdown") << false;
    QTest::newRow("shutdown-flush") << QString("shutdown") << true;
    QTest::newRow("receiver-write") << QString("receiver") << false;
    QTest::newRow("receiver-flush") << QString("receiver") << true;
    QTest::newRow("summary-write") << QString("summary") << false;
    QTest::newRow("summary-flush") << QString("summary") << true;
}

void DiagnosticLifecycleTest::terminalBoundaryFailureReturned() {
    QFETCH(QString, boundary);
    QFETCH(bool, flushFailure);
    auto storage = std::make_shared<MemoryStorage>();
    int handled = 0;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportFailure = [&](DiagnosticFailure) { ++handled; };
    diagnostics.start(std::move(start));
    diagnostics.enableFailureReporting();
    auto injectFailure = [&] {
        storage->file->failWrites = !flushFailure;
        storage->file->failFlush = flushFailure;
    };
    if (boundary == "shutdown")
        injectFailure();
    const auto failure = diagnostics.exitNormally([&] {
        injectFailure();
        if (boundary == "receiver")
            diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "receiver", "stop", {}, true));
        QCoreApplication::processEvents();
        QCOMPARE(handled, 0);
    });
    QVERIFY(failure.has_value());
    QCOMPARE(failure->kind, DiagnosticFailureKind::Write);
    QCOMPARE(failure->error, QStringLiteral("disk full"));
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(handled, 0);
    diagnostics.enableFailureReporting();
    QCoreApplication::processEvents();
    QCOMPARE(handled, 0);
    QVERIFY(!diagnostics.exitNormally({}));
}

QTEST_GUILESS_MAIN(DiagnosticLifecycleTest)
#include "DiagnosticLifecycleTest.moc"

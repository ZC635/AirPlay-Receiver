#include "app/DiagnosticLifecycle.h"

#include "diagnostics/DiagnosticSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

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

struct FileState {
    QByteArray bytes;
    QString ensureError;
    bool failWrites = false;
    bool failFlush = false;
    int writes = 0;
    int failWriteAt = 0;
    int closes = 0;
};

class MemoryFile final : public DiagnosticSessionFile {
public:
    explicit MemoryFile(std::shared_ptr<FileState> state) : state_(std::move(state)) {}
    qint64 write(const QByteArray &bytes) override {
        ++state_->writes;
        if (state_->failWrites || (state_->failWriteAt > 0 && state_->writes >= state_->failWriteAt))
            return -1;
        state_->bytes += bytes;
        return bytes.size();
    }
    bool flush() override { return !state_->failFlush; }
    void close() override { ++state_->closes; }
    QString errorString() const override { return QStringLiteral("disk full"); }
private:
    std::shared_ptr<FileState> state_;
};

class MemoryStorage final : public DiagnosticSessionStorage {
public:
    std::shared_ptr<FileState> file = std::make_shared<FileState>();
    QString ensureDirectory(const QString &) override { return file->ensureError; }
    std::unique_ptr<DiagnosticSessionFile> createExclusive(const QString &, QString *) override {
        return std::make_unique<MemoryFile>(file);
    }
    QVector<DiagnosticStoredFile> list(const QString &) override { return {}; }
    bool remove(const QString &) override { return true; }
};

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
    void consumedFailureIsNotReplayedAfterWindowAttachment();
    void environmentBaselineAvoidsSecondStartupCollection();
    void inactiveSessionSkipsEnvironmentAndMonitor_data();
    void inactiveSessionSkipsEnvironmentAndMonitor();
    void monitorRegistrationFailureIsRecordedAndCleanedUp();
    void normalExitKeepsBridgeAndMonitorThroughReceiverStop();
    void startupAbortRecordsReasonWithoutNormalMarker();
    void destructorOnlyCleansUp();
    void finalCloseFailureRemainsQueued_data();
    void finalCloseFailureRemainsQueued();
};

void DiagnosticLifecycleTest::normalLaunchCreatesNoLogButStillStopsReceiver() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticLifecycle diagnostics(QCoreApplication::instance());
    DiagnosticLifecycleStart start;
    start.applicationDirectory = directory.path();
    const auto result = diagnostics.start(std::move(start));
    QCOMPARE(result.childGate, DiagnosticChildGateResult::ContinueStartup);
    QVERIFY(result.creationError.isEmpty());
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(diagnostics.sink(), &nullDiagnosticLogSink());
    diagnostics.collectEnvironmentAndStartMonitor();
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
    start.reportWriteFailure = [&](QString text) { ++handled; error = std::move(text); };
    diagnostics.start(std::move(start));
    storage->file->failWrites = true;
    diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "before_window"));
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(handled, 0);
    QTRY_COMPARE(handled, 1);
    QCOMPARE(error, QStringLiteral("disk full"));
    diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "another_failure"));
    diagnostics.exitNormally({});
    QCoreApplication::processEvents();
    QCOMPARE(handled, 1);
}

void DiagnosticLifecycleTest::consumedFailureIsNotReplayedAfterWindowAttachment() {
    auto storage = std::make_shared<MemoryStorage>();
    bool attached = false;
    int callbacks = 0;
    int warnings = 0;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportWriteFailure = [&](QString) { ++callbacks; if (attached) ++warnings; };
    diagnostics.start(std::move(start));
    storage->file->failWrites = true;
    diagnostics.sink()->record(makeDiagnosticEvent(DiagnosticSeverity::Info, "startup", "before_window"));
    QCOMPARE(callbacks, 0);
    QCoreApplication::processEvents();
    QCOMPARE(callbacks, 1);
    attached = true;
    QCoreApplication::processEvents();
    QCOMPARE(callbacks, 1);
    QCOMPARE(warnings, 0);
}

void DiagnosticLifecycleTest::environmentBaselineAvoidsSecondStartupCollection() {
    QTemporaryDir directory;
    NetworkOperations network;
    int collections = 0;
    qint64 deadline = -1;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(),
        environmentDependencies(network, collections, deadline));
    diagnostics.start(enabledStart(directory.path()));
    QCOMPARE(collections, 0);
    QCOMPARE(network.registrations, 0);
    diagnostics.collectEnvironmentAndStartMonitor();
    QCOMPARE(collections, 1);
    QVERIFY(deadline > 0 && deadline <= 3000);
    QCOMPARE(network.registrations, 2);
    QCOMPARE(network.recollections, 0);
    QVERIFY(logBytes(directory.path()).contains("environment_snapshot"));
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

void DiagnosticLifecycleTest::finalCloseFailureRemainsQueued_data() {
    QTest::addColumn<bool>("flushFailure");
    QTest::newRow("write") << false;
    QTest::newRow("flush") << true;
}

void DiagnosticLifecycleTest::finalCloseFailureRemainsQueued() {
    QFETCH(bool, flushFailure);
    auto storage = std::make_shared<MemoryStorage>();
    int handled = 0;
    DiagnosticLifecycle diagnostics(QCoreApplication::instance(), memoryDependencies(storage));
    auto start = enabledStart(QStringLiteral("C:/test"));
    start.reportWriteFailure = [&](QString) { ++handled; };
    diagnostics.start(std::move(start));
    diagnostics.exitNormally([&] {
        storage->file->failWrites = !flushFailure;
        storage->file->failFlush = flushFailure;
    });
    QVERIFY(!diagnostics.loggingActive());
    QCOMPARE(handled, 0);
    QTRY_COMPARE(handled, 1);
}

QTEST_GUILESS_MAIN(DiagnosticLifecycleTest)
#include "DiagnosticLifecycleTest.moc"

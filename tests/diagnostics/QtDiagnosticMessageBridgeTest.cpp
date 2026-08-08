#include <QtTest>

#include "diagnostics/QtDiagnosticMessageBridge.h"
#include "support/CollectingDiagnosticLogSink.h"

#include <QMutex>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int previousHandlerCalls = 0;

class ScopedQtMessageHandler final {
public:
    explicit ScopedQtMessageHandler(QtMessageHandler handler) : m_previous(qInstallMessageHandler(handler)) {}
    ~ScopedQtMessageHandler() { qInstallMessageHandler(m_previous); }

private:
    QtMessageHandler m_previous;
};

void observingPreviousHandler(QtMsgType, const QMessageLogContext &, const QString &) {
    ++previousHandlerCalls;
}

struct BlockingPreviousState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    int calls = 0;

    bool waitForEntry() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }

    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};

BlockingPreviousState *blockingPreviousState = nullptr;

void blockingPreviousHandler(QtMsgType, const QMessageLogContext &, const QString &) {
    std::unique_lock lock(blockingPreviousState->mutex);
    ++blockingPreviousState->calls;
    blockingPreviousState->entered = true;
    blockingPreviousState->changed.notify_all();
    blockingPreviousState->changed.wait(lock, [] { return blockingPreviousState->released; });
}

class ThreadSafeSink final : public DiagnosticLogSink {
public:
    void record(DiagnosticEvent) override {
        QMutexLocker locker(&mutex);
        ++eventCount;
    }

    int size() const {
        QMutexLocker locker(&mutex);
        return eventCount;
    }

private:
    mutable QMutex mutex;
    int eventCount = 0;
};

} // namespace

class QtDiagnosticMessageBridgeTest final : public QObject {
    Q_OBJECT

private slots:
    void bridgeSanitizesAndRestoresPreviousHandler();
    void concurrentDefaultForwardingKeepsEveryEligibleMessage();
    void teardownWaitsForForwardedHandler();
    void qtMessageOutputUsesInstalledHandler();
    void routesAllowedQtMessagesAndFlushesBoundaries();
    void eventForMessageMapsFatalWithoutTerminating();
};

void QtDiagnosticMessageBridgeTest::bridgeSanitizesAndRestoresPreviousHandler() {
    CollectingSink sink;
    previousHandlerCalls = 0;
    const auto original = qInstallMessageHandler(observingPreviousHandler);
    qWarning().noquote() << "before";
    {
        QtDiagnosticMessageBridge bridge(&sink);
        qWarning().noquote() << "peer 192.168.1.55\nAuthorization: secret";
    }
    qWarning().noquote() << "after";
    qInstallMessageHandler(original);

    QCOMPARE(previousHandlerCalls, 3);
    QCOMPARE(sink.events.size(), 1);
    QCOMPARE(sink.events.first().component, QString("qt"));
    QVERIFY(sink.events.first().flushImmediately);
    QVERIFY(!sink.events.first().fields.value("message").contains("192.168.1.55"));
    QVERIFY(!sink.events.first().fields.value("message").contains("secret"));
}

void QtDiagnosticMessageBridgeTest::concurrentDefaultForwardingKeepsEveryEligibleMessage() {
    constexpr int workerCount = 6;
    constexpr int messagesPerWorker = 40;
    const QtMessageHandler original = qInstallMessageHandler(nullptr);
    ThreadSafeSink sink;
    {
        QtDiagnosticMessageBridge bridge(&sink);
        std::atomic<int> ready = 0;
        std::atomic<bool> start = false;
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (int worker = 0; worker != workerCount; ++worker) {
            workers.emplace_back([&] {
                ++ready;
                while (!start.load())
                    std::this_thread::yield();
                for (int message = 0; message != messagesPerWorker; ++message)
                    qWarning().noquote() << "concurrent default forwarding";
            });
        }
        while (ready.load() != workerCount)
            std::this_thread::yield();
        start = true;
        for (std::thread &worker : workers)
            worker.join();
    }
    qInstallMessageHandler(original);
    QCOMPARE(sink.size(), workerCount * messagesPerWorker);
}

void QtDiagnosticMessageBridgeTest::teardownWaitsForForwardedHandler() {
    BlockingPreviousState state;
    blockingPreviousState = &state;
    ScopedQtMessageHandler previous(&blockingPreviousHandler);
    auto bridge = std::make_unique<QtDiagnosticMessageBridge>(&nullDiagnosticLogSink());
    std::thread logging([] { qWarning().noquote() << "blocked previous handler"; });
    const bool entered = state.waitForEntry();
    std::atomic<bool> teardownStarted = false;
    std::atomic<bool> destroyed = false;
    std::thread teardown([&] {
        teardownStarted = true;
        bridge.reset();
        destroyed = true;
    });
    while (!teardownStarted.load())
        std::this_thread::yield();
    QTest::qWait(25);
    const bool completedBeforeRelease = destroyed.load();
    state.release();
    logging.join();
    teardown.join();
    blockingPreviousState = nullptr;

    QVERIFY(entered);
    QVERIFY(!completedBeforeRelease);
    QCOMPARE(state.calls, 1);
}

void QtDiagnosticMessageBridgeTest::qtMessageOutputUsesInstalledHandler() {
    previousHandlerCalls = 0;
    const QtMessageHandler original = qInstallMessageHandler(observingPreviousHandler);
    qt_message_output(QtWarningMsg, QMessageLogContext(), QStringLiteral("direct output"));
    qInstallMessageHandler(original);
    QCOMPARE(previousHandlerCalls, 1);
}

void QtDiagnosticMessageBridgeTest::routesAllowedQtMessagesAndFlushesBoundaries() {
    QMessageLogContext airplayContext("", 0, "", "airplay.receiver");
    QMessageLogContext otherContext("", 0, "", "other.component");
    QVERIFY(!QtDiagnosticMessageBridge::eventForMessage(QtDebugMsg, airplayContext, QStringLiteral("debug")).has_value());
    QVERIFY(QtDiagnosticMessageBridge::eventForMessage(QtInfoMsg, airplayContext, QStringLiteral("info")).has_value());
    QVERIFY(!QtDiagnosticMessageBridge::eventForMessage(QtInfoMsg, otherContext, QStringLiteral("info")).has_value());
    const auto warning = QtDiagnosticMessageBridge::eventForMessage(QtWarningMsg, otherContext, QStringLiteral("warning"));
    QVERIFY(warning.has_value());
    QCOMPARE(warning->severity, DiagnosticSeverity::Warning);
    QVERIFY(warning->flushImmediately);
    QCOMPARE(QtDiagnosticMessageBridge::eventForMessage(QtCriticalMsg, otherContext, QStringLiteral("critical"))->severity,
             DiagnosticSeverity::Error);
    QCOMPARE(QtDiagnosticMessageBridge::eventForMessage(QtFatalMsg, otherContext, QStringLiteral("fatal"))->severity,
             DiagnosticSeverity::Critical);
}

void QtDiagnosticMessageBridgeTest::eventForMessageMapsFatalWithoutTerminating() {
    const QMessageLogContext context("", 0, "", "airplay.fatal");
    const auto event = QtDiagnosticMessageBridge::eventForMessage(QtFatalMsg, context, QStringLiteral("fatal"));
    QVERIFY(event.has_value());
    QCOMPARE(event->name, QString("message"));
    QVERIFY(event->flushImmediately);
}

QTEST_GUILESS_MAIN(QtDiagnosticMessageBridgeTest)
#include "QtDiagnosticMessageBridgeTest.moc"

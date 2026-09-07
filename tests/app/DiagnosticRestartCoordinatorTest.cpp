#include <QtTest>

#include <QLocalSocket>
#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QFile>
#include <QUuid>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "app/DiagnosticActivation.h"
#include "app/DiagnosticRestartCoordinator.h"
#include "diagnostics/DiagnosticSession.h"

class DiagnosticRestartCoordinatorTest final : public QObject {
    Q_OBJECT

private slots:
    void startsChildWithPrivateHandshakeArguments();
    void emitsReadyOnlyAfterExactReadyLine();
    void errorIsSingleLineAndNeverReady();
    void rejectsStartFailureAndSecondBegin();
    void malformedOverflowAndTimeoutFailOnce();
    void rejectsMalformedAndSecondConnectionExactlyOnce();
    void incorrectServerNameCannotSignalReady();
    void destructorClosesListeningServer();
    void nonCoordinatedChildSkipsEveryGateOperation();
    void coordinatedChildWaitsForParentAfterReady();
    void coordinatedChildExitsForHandoffFailures();
    void sessionFailureSendsBoundedSanitizedErrorAndExits();
    void sessionFailureLocalizesOuterTextAndKeepsRawDetail();
    void normalCloseWritesSessionSummaryMarkers();
    void childWaitFailureClosesHandleAndRecordsHandoffFailure();
    void rejectsParentPidsOutsideWindowsHandleRange();
    void defaultChildReadySendCompletesWithoutAnEventLoop();
};

namespace {

DiagnosticRestartOperations operationsFor(QString token, QStringList *arguments = nullptr,
                                          bool startResult = true) {
    return {
        [startResult, arguments](const QString &, const QStringList &values, QString *) {
            if (arguments)
                *arguments = values;
            return startResult;
        },
        [token] { return token; },
    };
}

void connectAndWrite(const QString &token, const QByteArray &line) {
    QLocalSocket socket;
    socket.connectToServer(token);
    QVERIFY2(socket.waitForConnected(1000), qPrintable(socket.errorString()));
    QTest::qWait(1);
    QCOMPARE(socket.write(line), qint64(line.size()));
    QTest::qWait(1);
}

} // namespace

class DiagnosticActivationTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (QString::fromLatin1(context) == QStringLiteral("DiagnosticActivation")
            && QString::fromLatin1(sourceText) == QStringLiteral("Diagnostic logging could not be initialized: %1")) {
            return QStringLiteral("无法初始化诊断日志：%1");
        }
        return {};
    }
};

void DiagnosticRestartCoordinatorTest::startsChildWithPrivateHandshakeArguments() {
    QStringList arguments;
    DiagnosticRestartCoordinator coordinator(operationsFor(QStringLiteral("restart-test-args"), &arguments));

    QVERIFY(coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    QCOMPARE(arguments, QStringList({QStringLiteral("--diagnostic-log"),
                                    QStringLiteral("--diagnostic-parent-pid=1234"),
                                    QStringLiteral("--diagnostic-ready-token=restart-test-args")}));
}

void DiagnosticRestartCoordinatorTest::emitsReadyOnlyAfterExactReadyLine() {
    const QString token = QStringLiteral("restart-test-ready");
    DiagnosticRestartCoordinator coordinator(operationsFor(token));
    QSignalSpy ready(&coordinator, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy failed(&coordinator, &DiagnosticRestartCoordinator::failed);

    QVERIFY(coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    connectAndWrite(token, "READY\n");
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
}

void DiagnosticRestartCoordinatorTest::errorIsSingleLineAndNeverReady() {
    const QString token = QStringLiteral("restart-test-error");
    DiagnosticRestartCoordinator coordinator(operationsFor(token));
    QSignalSpy ready(&coordinator, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy failed(&coordinator, &DiagnosticRestartCoordinator::failed);

    QVERIFY(coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    connectAndWrite(token, "ERROR\tline one\r\nline two\n");
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    const QString error = failed.front().front().toString();
    QVERIFY(!error.contains('\r'));
    QVERIFY(!error.contains('\n'));
    QVERIFY(error.size() <= 1024);
}

void DiagnosticRestartCoordinatorTest::rejectsStartFailureAndSecondBegin() {
    DiagnosticRestartCoordinator failedStart(operationsFor(QStringLiteral("restart-test-fail"), nullptr, false));
    QSignalSpy ready(&failedStart, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy failures(&failedStart, &DiagnosticRestartCoordinator::failed);
    QVERIFY(!failedStart.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(ready.count(), 0);

    DiagnosticRestartCoordinator coordinator(operationsFor(QStringLiteral("restart-test-active")));
    QSignalSpy activeReady(&coordinator, &DiagnosticRestartCoordinator::childReady);
    QVERIFY(coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    QVERIFY(!coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    QCOMPARE(activeReady.count(), 0);
}

void DiagnosticRestartCoordinatorTest::malformedOverflowAndTimeoutFailOnce() {
    const QString token = QStringLiteral("restart-test-malformed");
    DiagnosticRestartCoordinator coordinator(operationsFor(token));
    QSignalSpy ready(&coordinator, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy failures(&coordinator, &DiagnosticRestartCoordinator::failed);
    QVERIFY(coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    connectAndWrite(token, QByteArray(1025, 'x'));
    QTRY_COMPARE(failures.count(), 1);
    QCOMPARE(ready.count(), 0);

    DiagnosticRestartCoordinator timeout(operationsFor(QStringLiteral("restart-test-timeout")));
    QSignalSpy timeoutReady(&timeout, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy timeoutFailures(&timeout, &DiagnosticRestartCoordinator::failed);
    QVERIFY(timeout.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    QTimer *timer = timeout.findChild<QTimer *>();
    QVERIFY(timer);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(timeoutFailures.count(), 1);
    QCOMPARE(timeoutReady.count(), 0);
}

void DiagnosticRestartCoordinatorTest::rejectsMalformedAndSecondConnectionExactlyOnce() {
    const QString malformedToken = QStringLiteral("restart-test-malformed-line");
    DiagnosticRestartCoordinator malformed(operationsFor(malformedToken));
    QSignalSpy malformedReady(&malformed, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy malformedFailures(&malformed, &DiagnosticRestartCoordinator::failed);
    QVERIFY(malformed.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    connectAndWrite(malformedToken, "NOT_READY\n");
    QTRY_COMPARE(malformedFailures.count(), 1);
    QCOMPARE(malformedReady.count(), 0);

    const QString token = QStringLiteral("restart-test-second-connection");
    DiagnosticRestartCoordinator coordinator(operationsFor(token));
    QSignalSpy ready(&coordinator, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy failures(&coordinator, &DiagnosticRestartCoordinator::failed);
    QVERIFY(coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    QLocalSocket first;
    first.connectToServer(token);
    QVERIFY(first.waitForConnected(1000));
    QTest::qWait(1);
    QLocalSocket second;
    second.connectToServer(token);
    QVERIFY(second.waitForConnected(1000));
    QTest::qWait(1);
    QTRY_COMPARE(failures.count(), 1);
    QTimer *timer = coordinator.findChild<QTimer *>();
    QVERIFY(timer);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(ready.count(), 0);
}

void DiagnosticRestartCoordinatorTest::incorrectServerNameCannotSignalReady() {
    const QString token = QStringLiteral("restart-test-private-name");
    DiagnosticRestartCoordinator coordinator(operationsFor(token));
    QSignalSpy ready(&coordinator, &DiagnosticRestartCoordinator::childReady);
    QSignalSpy failures(&coordinator, &DiagnosticRestartCoordinator::failed);
    QVERIFY(coordinator.begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    QLocalSocket socket;
    socket.connectToServer(QStringLiteral("restart-test-wrong-name"));
    QVERIFY(!socket.waitForConnected(100));
    QTimer *timer = coordinator.findChild<QTimer *>();
    QVERIFY(timer);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failures.count(), 1);
}

void DiagnosticRestartCoordinatorTest::destructorClosesListeningServer() {
    const QString token = QStringLiteral("restart-test-destructor");
    auto coordinator = std::make_unique<DiagnosticRestartCoordinator>(operationsFor(token));
    QVERIFY(coordinator->begin(QStringLiteral("C:/receiver/airplay_receiver.exe"), 1234));
    coordinator.reset();

    QLocalSocket socket;
    socket.connectToServer(token);
    QVERIFY(!socket.waitForConnected(100));
}

void DiagnosticRestartCoordinatorTest::nonCoordinatedChildSkipsEveryGateOperation() {
    int calls = 0;
    DiagnosticChildGateOperations operations;
    operations.openParentForWait = [&calls](qint64, QString *) { ++calls; return static_cast<void *>(nullptr); };
    operations.sendReadyLine = [&calls](const QString &, const QByteArray &, QString *) { ++calls; return false; };
    operations.waitForParentExit = [&calls](void *, int, QString *) { ++calls; return false; };
    operations.closeParentHandle = [&calls](void *) { ++calls; };

    const DiagnosticActivation activation = DiagnosticActivation::parse({"app", "--diagnostic-log"}, {});
    QCOMPARE(runDiagnosticChildGate(activation, nullptr, {}, operations),
             DiagnosticChildGateResult::ContinueStartup);
    QCOMPARE(calls, 0);
}

void DiagnosticRestartCoordinatorTest::coordinatedChildWaitsForParentAfterReady() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(options);
    QVERIFY(created.session);
    const DiagnosticActivation activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-parent-pid=1234", "--diagnostic-ready-token=private"}, {});
    QStringList order;
    int timeout = 0;
    qint64 receivedPid = 0;
    QString receivedToken;
    QByteArray receivedLine;
    void *handle = reinterpret_cast<void *>(quintptr(1));
    DiagnosticChildGateOperations gateOperations {
        [&order, &receivedPid, handle](qint64 pid, QString *) { order.append(QStringLiteral("open")); receivedPid = pid; return handle; },
        [&order, &receivedToken, &receivedLine](const QString &token, const QByteArray &line, QString *) { order.append(QStringLiteral("ready")); receivedToken = token; receivedLine = line; return true; },
        [&order, &timeout](void *, int timeoutMs, QString *) { order.append(QStringLiteral("wait")); timeout = timeoutMs; return true; },
        [&order](void *) { order.append(QStringLiteral("close")); },
    };

    QCOMPARE(runDiagnosticChildGate(activation, created.session.get(), {}, gateOperations),
             DiagnosticChildGateResult::ContinueStartup);
    QCOMPARE(order, QStringList({QStringLiteral("open"), QStringLiteral("ready"),
                                 QStringLiteral("wait"), QStringLiteral("close")}));
    QCOMPARE(receivedPid, qint64(1234));
    QCOMPARE(receivedToken, QStringLiteral("private"));
    QCOMPARE(receivedLine, QByteArrayLiteral("READY\n"));
    QCOMPARE(timeout, 30000);
}

void DiagnosticRestartCoordinatorTest::coordinatedChildExitsForHandoffFailures() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(options);
    QVERIFY(created.session);
    const DiagnosticActivation activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-parent-pid=1234", "--diagnostic-ready-token=private"}, {});
    QString sent;
    int closes = 0;
    DiagnosticChildGateOperations openFailure {
        [](qint64, QString *error) { *error = QStringLiteral("access denied"); return static_cast<void *>(nullptr); },
        [&sent](const QString &, const QByteArray &line, QString *) { sent = QString::fromUtf8(line); return true; }, {}, {}};
    QCOMPARE(runDiagnosticChildGate(activation, created.session.get(), {}, openFailure),
             DiagnosticChildGateResult::ExitChild);
    QVERIFY(sent.startsWith(QStringLiteral("ERROR\t")));

    void *handle = reinterpret_cast<void *>(quintptr(1));
    DiagnosticChildGateOperations sendFailure {
        [handle](qint64, QString *) { return handle; },
        [](const QString &, const QByteArray &, QString *error) { *error = QStringLiteral("channel unavailable"); return false; },
        {}, [&closes](void *) { ++closes; }};
    QCOMPARE(runDiagnosticChildGate(activation, created.session.get(), {}, sendFailure),
             DiagnosticChildGateResult::ExitChild);
    QCOMPARE(closes, 1);
}

void DiagnosticRestartCoordinatorTest::sessionFailureSendsBoundedSanitizedErrorAndExits() {
    const DiagnosticActivation activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-parent-pid=1234", "--diagnostic-ready-token=private"}, {});
    QByteArray line;
    DiagnosticChildGateOperations operations;
    operations.sendReadyLine = [&line](const QString &, const QByteArray &value, QString *) { line = value; return true; };

    QCOMPARE(runDiagnosticChildGate(activation, nullptr,
                                    QStringLiteral("bad\r\nC:/secret/path") + QString(2000, QLatin1Char('x')),
                                    operations), DiagnosticChildGateResult::ExitChild);
    QVERIFY(line.startsWith(QByteArrayLiteral("ERROR\t")));
    QVERIFY(line.endsWith('\n'));
    QVERIFY(line.size() <= 1024);
    QVERIFY(!line.contains('\r'));
}

void DiagnosticRestartCoordinatorTest::sessionFailureLocalizesOuterTextAndKeepsRawDetail() {
    const DiagnosticActivation activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-parent-pid=1234", "--diagnostic-ready-token=private"}, {});
    QByteArray line;
    DiagnosticChildGateOperations operations;
    operations.sendReadyLine = [&line](const QString &, const QByteArray &value, QString *) { line = value; return true; };

    DiagnosticActivationTranslator translator;
    QCoreApplication::installTranslator(&translator);
    QCOMPARE(runDiagnosticChildGate(activation, nullptr, QStringLiteral("raw session detail"), operations),
             DiagnosticChildGateResult::ExitChild);
    QCoreApplication::removeTranslator(&translator);

    QCOMPARE(QString::fromUtf8(line), QStringLiteral("ERROR\t无法初始化诊断日志：raw session detail\n"));
}

void DiagnosticRestartCoordinatorTest::normalCloseWritesSessionSummaryMarkers() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(options);
    QVERIFY(created.session);
    const QString logPath = created.session->filePath();
    created.session->closeNormally();

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QByteArray contents = log.readAll();
    QVERIFY(!contents.contains("session_closed_normally"));
    QVERIFY(contents.contains(" session session_summary"));
    QVERIFY(contents.contains("normal_exit=yes"));
}

void DiagnosticRestartCoordinatorTest::childWaitFailureClosesHandleAndRecordsHandoffFailure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(options);
    QVERIFY(created.session);
    const QString logPath = created.session->filePath();
    const DiagnosticActivation activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-parent-pid=1234", "--diagnostic-ready-token=private"}, {});
    void *handle = reinterpret_cast<void *>(quintptr(1));
    int timeout = 0;
    int closes = 0;
    DiagnosticChildGateOperations operations {
        [handle](qint64, QString *) { return handle; },
        [](const QString &, const QByteArray &line, QString *) { return line == QByteArrayLiteral("READY\n"); },
        [&timeout](void *, int timeoutMs, QString *) { timeout = timeoutMs; return false; },
        [&closes, handle](void *value) { QCOMPARE(value, handle); ++closes; },
    };

    QCOMPARE(runDiagnosticChildGate(activation, created.session.get(), {}, operations),
             DiagnosticChildGateResult::ExitChild);
    QCOMPARE(timeout, 30000);
    QCOMPARE(closes, 1);
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QByteArray contents = log.readAll();
    QVERIFY(contents.contains(" ERROR startup child_handoff_failed"));
    QVERIFY(contents.contains("reason=parent_exit_wait_failed result=failed"));
    QVERIFY(!contents.contains(" session session_summary"));
    QVERIFY(!contents.contains("normal_exit=yes"));
}

void DiagnosticRestartCoordinatorTest::rejectsParentPidsOutsideWindowsHandleRange() {
    for (const QString &parentPid : {QStringLiteral("4294967296"), QStringLiteral("4294967297")}) {
        const DiagnosticActivation activation = DiagnosticActivation::parse(
            {"app", "--diagnostic-log", "--diagnostic-parent-pid=" + parentPid,
             "--diagnostic-ready-token=private"}, {});
        QVERIFY(!activation.isCoordinatedChild());
        QCOMPARE(activation.argumentError, QStringLiteral("invalid_diagnostic_parent_pid"));

        int openCalls = 0;
        DiagnosticChildGateOperations operations;
        operations.openParentForWait = [&openCalls](qint64, QString *) {
            ++openCalls;
            return reinterpret_cast<void *>(quintptr(1));
        };
        QCOMPARE(runDiagnosticChildGate(activation, nullptr, {}, operations),
                 DiagnosticChildGateResult::ContinueStartup);
        QCOMPARE(openCalls, 0);
    }

    const DiagnosticActivation maximum = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-parent-pid=4294967295",
         "--diagnostic-ready-token=private"}, {});
    QVERIFY(maximum.isCoordinatedChild());
    QCOMPARE(maximum.parentPid, qint64(4294967295));
}

void DiagnosticRestartCoordinatorTest::defaultChildReadySendCompletesWithoutAnEventLoop() {
    const QString token = QStringLiteral("restart-test-default-send-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(options);
    QVERIFY(created.session);
    std::mutex serverMutex;
    std::condition_variable serverListening;
    bool listenComplete = false;
    bool listening = false;
    QByteArray received;
    std::thread serverThread([&] {
        QLocalServer server;
        const bool started = server.listen(token);
        {
            std::lock_guard<std::mutex> lock(serverMutex);
            listening = started;
            listenComplete = true;
        }
        serverListening.notify_one();
        if (!started || !server.waitForNewConnection(2000))
            return;
        std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
        if (socket && socket->waitForReadyRead(1000))
            received = socket->readAll();
    });
    bool serverReady = false;
    {
        std::unique_lock<std::mutex> lock(serverMutex);
        serverReady = serverListening.wait_for(lock, std::chrono::seconds(2),
                                               [&] { return listenComplete; }) && listening;
    }
    if (!serverReady) {
        serverThread.join();
        QFAIL("Local readiness server could not listen.");
    }
    const DiagnosticActivation activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-parent-pid=1234",
         "--diagnostic-ready-token=" + token}, {});
    void *handle = reinterpret_cast<void *>(quintptr(1));
    int waits = 0;
    int timeout = 0;
    int closes = 0;
    DiagnosticChildGateOperations operations;
    operations.openParentForWait = [handle](qint64, QString *) { return handle; };
    operations.waitForParentExit = [&waits, &timeout](void *, int timeoutMs, QString *) {
        ++waits;
        timeout = timeoutMs;
        return true;
    };
    operations.closeParentHandle = [&closes, handle](void *value) {
        QCOMPARE(value, handle);
        ++closes;
    };

    const DiagnosticChildGateResult result =
        runDiagnosticChildGate(activation, created.session.get(), {}, operations);
    serverThread.join();
    QCOMPARE(result, DiagnosticChildGateResult::ContinueStartup);
    QCOMPARE(waits, 1);
    QCOMPARE(timeout, 30000);
    QCOMPARE(closes, 1);
    QCOMPARE(received, QByteArrayLiteral("READY\n"));
}

QTEST_GUILESS_MAIN(DiagnosticRestartCoordinatorTest)
#include "DiagnosticRestartCoordinatorTest.moc"

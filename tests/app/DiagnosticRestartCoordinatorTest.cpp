#include <QtTest>

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QFile>

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
    void childWaitFailureClosesHandleAndRecordsHandoffFailure();
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
    QVERIFY(!contents.contains("session_closed_normally"));
}

QTEST_GUILESS_MAIN(DiagnosticRestartCoordinatorTest)
#include "DiagnosticRestartCoordinatorTest.moc"

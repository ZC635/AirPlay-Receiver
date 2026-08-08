#include <QtTest>

#include "diagnostics/QtDiagnosticMessageBridge.h"
#include "support/CollectingDiagnosticLogSink.h"

namespace {

int previousHandlerCalls = 0;

void observingPreviousHandler(QtMsgType, const QMessageLogContext &, const QString &) {
    ++previousHandlerCalls;
}

} // namespace

class QtDiagnosticMessageBridgeTest final : public QObject {
    Q_OBJECT

private slots:
    void bridgeSanitizesAndRestoresPreviousHandler();
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

#include <QtTest>

#include "app/DiagnosticActivation.h"

class DiagnosticActivationTest final : public QObject {
    Q_OBJECT

private slots:
    void normalLaunchIsDisabled();
    void commandArgumentWinsOverEnvironmentCompatibilityFlag();
    void nonEmptyEnvironmentActivatesUnifiedMode();
    void parsesPrivateParentGateWithoutLoggingToken();
    void rejectsInvalidOrIncompletePrivateArguments();
    void ignoresOrdinaryStartupArguments();
};

void DiagnosticActivationTest::normalLaunchIsDisabled() {
    const auto activation = DiagnosticActivation::parse({"airplay_receiver.exe"}, {});
    QVERIFY(!activation.enabled);
    QCOMPARE(activation.source, DiagnosticActivationSource::None);
}

void DiagnosticActivationTest::commandArgumentWinsOverEnvironmentCompatibilityFlag() {
    const auto activation = DiagnosticActivation::parse(
        {"airplay_receiver.exe", "--diagnostic-log"}, "legacy-value");
    QVERIFY(activation.enabled);
    QCOMPARE(activation.source, DiagnosticActivationSource::CommandArgument);
    QCOMPARE(activation.sourceName(), QString("command_argument"));
}

void DiagnosticActivationTest::nonEmptyEnvironmentActivatesUnifiedMode() {
    const auto activation = DiagnosticActivation::parse({"airplay_receiver.exe"}, "1");
    QVERIFY(activation.enabled);
    QCOMPARE(activation.source, DiagnosticActivationSource::EnvironmentVariable);
}

void DiagnosticActivationTest::parsesPrivateParentGateWithoutLoggingToken() {
    const auto activation = DiagnosticActivation::parse(
        {"airplay_receiver.exe", "--diagnostic-log", "--diagnostic-parent-pid=42",
         "--diagnostic-ready-token=9f17"}, {});
    QCOMPARE(activation.parentPid, qint64(42));
    QCOMPARE(activation.readyToken, QString("9f17"));
    QVERIFY(!activation.publicFields().values().contains("9f17"));
}

void DiagnosticActivationTest::rejectsInvalidOrIncompletePrivateArguments() {
    QCOMPARE(DiagnosticActivation::parse({"app", "--diagnostic-parent-pid=bad"}, {}).argumentError,
             QString("invalid_diagnostic_parent_pid"));
    QCOMPARE(DiagnosticActivation::parse({"app", "--diagnostic-ready-token=a",
                                          "--diagnostic-ready-token=b"}, {}).argumentError,
             QString("duplicate_diagnostic_ready_token"));
    QCOMPARE(DiagnosticActivation::parse({"app", "--diagnostic-parent-pid=42"}, {}).argumentError,
             QString("incomplete_diagnostic_coordination"));
}

void DiagnosticActivationTest::ignoresOrdinaryStartupArguments() {
    const auto activation = DiagnosticActivation::parse({"app", "--fullscreen", "--other=value"}, {});
    QVERIFY(activation.argumentError.isEmpty());
    QVERIFY(!activation.enabled);
}

QTEST_GUILESS_MAIN(DiagnosticActivationTest)
#include "DiagnosticActivationTest.moc"

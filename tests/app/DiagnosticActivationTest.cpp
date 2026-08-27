#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "app/DiagnosticActivation.h"

#define main diagnosticActivationTestMain
#include "app/main.cpp"
#undef main

class DiagnosticActivationTest final : public QObject {
    Q_OBJECT

private slots:
    void normalLaunchIsDisabled();
    void commandArgumentWinsOverEnvironmentCompatibilityFlag();
    void nonEmptyEnvironmentActivatesUnifiedMode();
    void parsesPrivateParentGateWithoutLoggingToken();
    void rejectsInvalidOrIncompletePrivateArguments();
    void rejectsPrivateCoordinationWithoutActivation();
    void rejectsInvalidAndDuplicateParentPids();
    void repeatedDiagnosticFlagIsIdempotent();
    void ignoresOrdinaryStartupArguments();
    void directCreationFailureFallsBackToNullMode();
    void successfulCreationEnablesTitleSuffix();
    void missingSettingsFileIsDefaulted();
    void invalidSettingsFileIsDefaulted();
    void validSettingsObjectIsLoaded();
    void missingRuntimeAbortsWithoutNormalDiagnosticClose();
    void skippedStandaloneRuntimeAvoidsManifestEntries();
    void incompatibleRuntimePathStopsWithActionableMessage();
    void compatibleRuntimePathContinuesWithoutStartupError();
    void runtimePathCompatibilityFieldsAvoidPathDisclosure();
    void incompletePortableManifestSkipsGStreamerSetupAndProbe();
    void completePortableManifestConfiguresGStreamerBeforeProbing();
    void skippedPortableManifestConfiguresGStreamerBeforeProbing();
    void missingCorePluginsInNonAsciiPathReportsPathLoadFailure();
    void missingCorePluginsInAsciiPathReportsPluginLoadFailure();
    void readyCorePluginsContinueStartup();
    void pluginReadinessFieldsAreBoundedAndPathFree();
    void inactiveCreatedSessionDoesNotEnableDiagnosticTitle();
    void queuedPreWindowFailureReachesOneConfiguredHandler();
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

void DiagnosticActivationTest::rejectsPrivateCoordinationWithoutActivation() {
    const auto activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-parent-pid=42", "--diagnostic-ready-token=private"}, {});
    QCOMPARE(activation.argumentError, QString("diagnostic_coordination_requires_activation"));
    QVERIFY(!activation.enabled);
    QVERIFY(!activation.isCoordinatedChild());
    QCOMPARE(activation.parentPid, qint64(0));
    QVERIFY(activation.readyToken.isEmpty());
}

void DiagnosticActivationTest::rejectsInvalidAndDuplicateParentPids() {
    for (const QString &argument : {QStringLiteral("--diagnostic-parent-pid=0"),
                                    QStringLiteral("--diagnostic-parent-pid=-1"),
                                    QStringLiteral("--diagnostic-parent-pid=9223372036854775808")}) {
        QCOMPARE(DiagnosticActivation::parse({"app", argument}, {}).argumentError,
                 QString("invalid_diagnostic_parent_pid"));
    }
    QCOMPARE(DiagnosticActivation::parse(
                 {"app", "--diagnostic-parent-pid=1", "--diagnostic-parent-pid=2"}, {})
                 .argumentError,
             QString("duplicate_diagnostic_parent_pid"));
}

void DiagnosticActivationTest::repeatedDiagnosticFlagIsIdempotent() {
    const auto activation = DiagnosticActivation::parse(
        {"app", "--diagnostic-log", "--diagnostic-log"}, {});
    QVERIFY(activation.enabled);
    QCOMPARE(activation.source, DiagnosticActivationSource::CommandArgument);
    QVERIFY(activation.argumentError.isEmpty());
}

void DiagnosticActivationTest::ignoresOrdinaryStartupArguments() {
    const auto activation = DiagnosticActivation::parse({"app", "--fullscreen", "--other=value"}, {});
    QVERIFY(activation.argumentError.isEmpty());
    QVERIFY(!activation.enabled);
}

void DiagnosticActivationTest::directCreationFailureFallsBackToNullMode() {
    const auto decision = diagnosticStartupDecision(
        DiagnosticActivation{true, DiagnosticActivationSource::CommandArgument},
        false, "access denied");
    QVERIFY(!decision.loggingActive);
    QVERIFY(decision.continueApplication);
    QCOMPARE(decision.userError, QString("access denied"));
}

void DiagnosticActivationTest::successfulCreationEnablesTitleSuffix() {
    const auto decision = diagnosticStartupDecision(
        DiagnosticActivation{true, DiagnosticActivationSource::CommandArgument}, true, {});
    QVERIFY(decision.loggingActive);
    QVERIFY(decision.continueApplication);
    QVERIFY(decision.userError.isEmpty());
}

void DiagnosticActivationTest::missingSettingsFileIsDefaulted() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QVERIFY(!settingsFileContainsObject(directory.filePath("airplay-settings.json")));
}

void DiagnosticActivationTest::invalidSettingsFileIsDefaulted() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath("airplay-settings.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not json");
    file.close();

    QVERIFY(!settingsFileContainsObject(file.fileName()));
}

void DiagnosticActivationTest::validSettingsObjectIsLoaded() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath("airplay-settings.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();

    QVERIFY(settingsFileContainsObject(file.fileName()));
}

void DiagnosticActivationTest::missingRuntimeAbortsWithoutNormalDiagnosticClose() {
    const auto decision = diagnosticShutdownDecision(false);

    QVERIFY(decision.recordStartupAborted);
    QVERIFY(!decision.recordShutdownStarted);
    QVERIFY(!decision.closeNormally);
}

void DiagnosticActivationTest::skippedStandaloneRuntimeAvoidsManifestEntries() {
    const auto decision = diagnosticStandaloneRuntimeDecision(false);

    QVERIFY(!decision.emitManifestEntries);
    QCOMPARE(decision.result, QString("skipped"));
}

void DiagnosticActivationTest::incompatibleRuntimePathStopsWithActionableMessage() {
    const auto decision = runtimePathStartupDecision({false, true, 936, 42});

    QVERIFY(!decision.continueApplication);
    QCOMPARE(decision.title, QString("Unsupported application path"));
    QVERIFY(decision.message.contains(QStringLiteral("GStreamer")));
    QVERIFY(decision.message.contains(QStringLiteral("C:\\AirPlay")));
    QCOMPARE(decision.abortReason, QString("unsupported_application_path"));
}

void DiagnosticActivationTest::compatibleRuntimePathContinuesWithoutStartupError() {
    const auto decision = runtimePathStartupDecision({true, false, 936, 10});

    QVERIFY(decision.continueApplication);
    QVERIFY(decision.title.isEmpty());
    QVERIFY(decision.message.isEmpty());
    QVERIFY(decision.abortReason.isEmpty());
}

void DiagnosticActivationTest::runtimePathCompatibilityFieldsAvoidPathDisclosure() {
    const QMap<QString, QString> fields = runtimePathCompatibilityFields({false, true, 936, 42});

    QCOMPARE(fields.keys(), QStringList({QStringLiteral("ansi_code_page"),
                                         QStringLiteral("has_non_ascii"),
                                         QStringLiteral("path_length"),
                                         QStringLiteral("result")}));
    QCOMPARE(fields.value(QStringLiteral("result")), QStringLiteral("no"));
    QCOMPARE(fields.value(QStringLiteral("has_non_ascii")), QStringLiteral("yes"));
    QCOMPARE(fields.value(QStringLiteral("ansi_code_page")), QStringLiteral("936"));
    QCOMPARE(fields.value(QStringLiteral("path_length")), QStringLiteral("42"));
}

void DiagnosticActivationTest::incompletePortableManifestSkipsGStreamerSetupAndProbe() {
    QStringList calls;

    const bool continues = runPortableGStreamerStartupSequence(
        true,
        [&calls] {
            calls << QStringLiteral("manifest");
            return false;
        },
        [&calls] { calls << QStringLiteral("configure"); },
        [&calls] {
            calls << QStringLiteral("probe");
            return true;
        });

    QVERIFY(!continues);
    QCOMPARE(calls, QStringList({QStringLiteral("manifest")}));
}

void DiagnosticActivationTest::completePortableManifestConfiguresGStreamerBeforeProbing() {
    QStringList calls;

    const bool continues = runPortableGStreamerStartupSequence(
        true,
        [&calls] {
            calls << QStringLiteral("manifest");
            return true;
        },
        [&calls] { calls << QStringLiteral("configure"); },
        [&calls] {
            calls << QStringLiteral("probe");
            return true;
        });

    QVERIFY(continues);
    QCOMPARE(calls, QStringList({QStringLiteral("manifest"), QStringLiteral("configure"),
                                 QStringLiteral("probe")}));
}

void DiagnosticActivationTest::skippedPortableManifestConfiguresGStreamerBeforeProbing() {
    QStringList calls;

    const bool continues = runPortableGStreamerStartupSequence(
        false,
        [&calls] {
            calls << QStringLiteral("manifest");
            return false;
        },
        [&calls] { calls << QStringLiteral("configure"); },
        [&calls] {
            calls << QStringLiteral("probe");
            return true;
        });

    QVERIFY(continues);
    QCOMPARE(calls, QStringList({QStringLiteral("configure"), QStringLiteral("probe")}));
}

void DiagnosticActivationTest::missingCorePluginsInNonAsciiPathReportsPathLoadFailure() {
    const GStreamerPluginReadiness readiness{
        false, {"app", "libav", "playback", "autodetect", "videoparsersbad"}, {}};
    const auto decision = gstreamerPluginStartupDecision(readiness, true);

    QVERIFY(!decision.continueApplication);
    QCOMPARE(decision.title, QString("GStreamer plugins unavailable"));
    QVERIFY(decision.message.contains(QString("app, libav, playback, autodetect, videoparsersbad")));
    QVERIFY(decision.message.contains(QStringLiteral("C:\\AirPlay")));
    QCOMPARE(decision.reason, QString("path_load_failure"));
    QCOMPARE(decision.abortReason, QString("gstreamer_plugin_path_load_failure"));
}

void DiagnosticActivationTest::missingCorePluginsInAsciiPathReportsPluginLoadFailure() {
    const GStreamerPluginReadiness readiness{false, {"app", "libav"}, {}};
    const auto decision = gstreamerPluginStartupDecision(readiness, false);

    QVERIFY(!decision.continueApplication);
    QVERIFY(decision.message.contains(QString("re-extract"), Qt::CaseInsensitive));
    QCOMPARE(decision.reason, QString("plugin_load_failure"));
    QCOMPARE(decision.abortReason, QString("gstreamer_plugin_load_failure"));
}

void DiagnosticActivationTest::readyCorePluginsContinueStartup() {
    const auto decision = gstreamerPluginStartupDecision({true, {}, {}}, true);

    QVERIFY(decision.continueApplication);
    QCOMPARE(decision.reason, QString("ready"));
}

void DiagnosticActivationTest::pluginReadinessFieldsAreBoundedAndPathFree() {
    const GStreamerPluginReadiness readiness{false, {"app", "libav"}, {}};
    const auto decision = gstreamerPluginStartupDecision(readiness, true);
    const auto fields = gstreamerPluginReadinessFields(readiness, true, decision.reason);

    QCOMPARE(fields.value("result"), QString("no"));
    QCOMPARE(fields.value("missing_count"), QString("2"));
    QCOMPARE(fields.value("missing_plugins"), QString("app,libav"));
    QCOMPARE(fields.value("has_non_ascii"), QString("yes"));
    QCOMPARE(fields.value("reason"), QString("path_load_failure"));
    QVERIFY(!fields.values().join(' ').contains(QStringLiteral("C:\\")));
}

void DiagnosticActivationTest::inactiveCreatedSessionDoesNotEnableDiagnosticTitle() {
    QVERIFY(!diagnosticLoggingActiveForSession(true, false));
}

void DiagnosticActivationTest::queuedPreWindowFailureReachesOneConfiguredHandler() {
    DiagnosticWriteFailureRelay relay;
    int handled = 0;
    QMetaObject::invokeMethod(QCoreApplication::instance(), [&relay] {
        relay.deliver("disk full");
    }, Qt::QueuedConnection);
    relay.setHandler([&handled](QString) { ++handled; });

    QTRY_COMPARE(handled, 1);
}

QTEST_GUILESS_MAIN(DiagnosticActivationTest)
#include "DiagnosticActivationTest.moc"

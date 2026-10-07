#include <QtTest>

#include <QFile>
#include <QTemporaryDir>
#include <QTranslator>

#include "app/DiagnosticActivation.h"
#include "app/LanguageManager.h"
#include "diagnostics/DiagnosticSession.h"

#define main diagnosticActivationTestMain
#include "app/main.cpp"
#undef main

namespace {

class StartupTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (QString::fromLatin1(context) != QStringLiteral("Startup")) {
            return {};
        }
        const QString source = QString::fromLatin1(sourceText);
        if (source == QStringLiteral("Diagnostic logging could not be started: %1")) {
            return QString::fromUtf8(u8"无法启动诊断日志：%1");
        }
        if (source == QStringLiteral("Diagnostic logging stopped: %1")) {
            return QString::fromUtf8(u8"诊断日志已停止：%1");
        }
        if (source == QStringLiteral("Diagnostic log may be incomplete: %1")) {
            return QString::fromUtf8(u8"诊断日志可能不完整：%1");
        }
        if (source == QStringLiteral("Diagnostic log could not be fully saved: %1")) {
            return QString::fromUtf8(u8"诊断日志未能完整保存：%1");
        }
        return {};
    }
};

class InstalledTranslator final {
public:
    explicit InstalledTranslator(QTranslator *translator)
        : translator_(translator) {
        QCoreApplication::installTranslator(translator_);
    }

    ~InstalledTranslator() {
        QCoreApplication::removeTranslator(translator_);
    }

private:
    QTranslator *translator_;
};

} // namespace

class DiagnosticActivationTest final : public QObject {
    Q_OBJECT

private slots:
    void registryOnlyMissingReachesPreparationButDllMissingDoesNot();
    void receiverStartWaitsForActualCoreCheck();
    void ordinaryStartupSkipsPreflight();
    void ordinaryStartupRejectsIsolationFailure();
    void ordinaryPrivateCacheIsOwnedAndReadinessUnknown();
    void normalLaunchIsDisabled();
    void commandArgumentWinsOverEnvironmentCompatibilityFlag();
    void nonEmptyEnvironmentActivatesUnifiedMode();
    void parsesPrivateParentGateWithoutLoggingToken();
    void rejectsInvalidOrIncompletePrivateArguments();
    void rejectsPrivateCoordinationWithoutActivation();
    void rejectsInvalidAndDuplicateParentPids();
    void repeatedDiagnosticFlagIsIdempotent();
    void ignoresOrdinaryStartupArguments();
    void missingSettingsFileIsDefaulted();
    void invalidSettingsFileIsDefaulted();
    void validSettingsObjectIsLoaded();
    void savedChineseLanguageIsAppliedBeforeDiagnosticStartup();
    void missingStartupCatalogFallsBackToEnglishAndRecordsOneDiagnosticEvent();
    void startupTranslationFailureSurvivesPrivacyFiltering();
    void startupTranslationFailureRedactsUnsupportedSelection();
    void startupSettingsSnapshotSurvivesPrivacyFiltering_data();
    void startupSettingsSnapshotSurvivesPrivacyFiltering();
    void diagnosticLoggingBodiesTranslateWithoutChangingRawDetails();
    void skippedStandaloneRuntimeAvoidsManifestEntries();
    void incompatibleRuntimePathStopsWithActionableMessage();
    void compatibleRuntimePathContinuesWithoutStartupError();
    void runtimePathCompatibilityFieldsAvoidPathDisclosure();
    void runtimePathCharacterCountSurvivesPrivacyFiltering();
    void incompletePortableManifestSkipsGStreamerSetupAndProbe();
    void completePortableManifestConfiguresGStreamerBeforeProbing();
    void skippedPortableManifestConfiguresGStreamerBeforeProbing();
    void missingCorePluginsInNonAsciiPathReportsPathLoadFailure();
    void missingCorePluginsInAsciiPathReportsPluginLoadFailure();
    void readyCorePluginsContinueStartup();
    void pluginReadinessFieldsAreBoundedAndPathFree();
};

void DiagnosticActivationTest::registryOnlyMissingReachesPreparationButDllMissingDoesNot() {
    StandaloneRuntimeSnapshot snapshot;
    snapshot.complete=false;
    snapshot.missingRelativePaths={"gstreamer-1.0/registry.x86_64.bin"};
    QVERIFY(portableManifestAllowsPreparation(snapshot));
    snapshot.missingRelativePaths.append("Qt6Core.dll");
    QVERIFY(!portableManifestAllowsPreparation(snapshot));
}
void DiagnosticActivationTest::receiverStartWaitsForActualCoreCheck() {
    QStringList calls;
    bool receiverStarted=false;
    const bool ready=runPortableGStreamerStartupSequence(true,
        [&]{calls<<"manifest";return true;},[&]{calls<<"prepare";return true;},
        [&]{calls<<"configure_private";},[&]{calls<<"actual_core";return false;});
    if(ready)receiverStarted=true;
    QCOMPARE(calls,QStringList({"manifest","prepare","configure_private","actual_core"}));
    QVERIFY(!receiverStarted);
    calls.clear();
    QVERIFY(!runPortableGStreamerStartupSequence(true,[]{return true;},[]{return false;},
        [&]{calls<<"configure_private";},[&]{calls<<"actual_core";return true;}));
    QVERIFY(calls.isEmpty());
}

void DiagnosticActivationTest::ordinaryStartupSkipsPreflight() {
    for (bool portable : {false, true}) {
        QStringList calls;
        QVERIFY(runPortableGStreamerStartupSequence(portable,
            [&]{ calls << "manifest"; return true; },
            [&]{ calls << "prepare"; return false; },
            [&]{ calls << "configure"; },
            [&]{ calls << "core"; return false; }, false,
            [&]{ calls << "isolate"; return true; }));
        QCOMPARE(calls, portable ? QStringList({"manifest", "isolate", "configure"})
                                 : QStringList({"configure"}));
    }
}
void DiagnosticActivationTest::ordinaryStartupRejectsIsolationFailure() {
    bool configured = false;
    QVERIFY(!runPortableGStreamerStartupSequence(true, []{return true;}, []{return true;},
        [&]{configured = true;}, []{return true;}, false, []{return false;}));
    QVERIFY(!configured);
}

void DiagnosticActivationTest::ordinaryPrivateCacheIsOwnedAndReadinessUnknown() {
#if AIRPLAY_WITH_UXPLAY
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString package = directory.filePath("package");
    QVERIFY(QDir().mkpath(package + "/gstreamer-1.0"));
    const QString source = package + "/gstreamer-1.0/registry.x86_64.bin";
    const QString neighbor = directory.filePath("neighbor.bin");
    for (const QString &path : {source, neighbor}) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("sentinel"), qint64(8));
    }
    for (bool missing : {false, true}) {
        if (missing) QVERIFY(QFile::remove(source));
        auto result = seedOrdinaryRuntime(package, directory.filePath("temp"));
        QVERIFY2(result.runtime != nullptr, qPrintable(result.reason));
        QCOMPARE(result.readinessState, ReadinessState::Unknown);
        QVERIFY(!result.readiness.ready);
        const QString ownedRoot = QFileInfo(result.runtime->registryPath()).absolutePath();
        QFile seeded(result.runtime->registryPath()); QVERIFY(seeded.open(QIODevice::ReadOnly));
        QCOMPARE(seeded.readAll(), missing ? QByteArray() : QByteArray("sentinel"));
        seeded.close();
        QVERIFY(result.runtime->close().complete);
        QVERIFY(!QFileInfo::exists(ownedRoot));
        QFile preserved(neighbor); QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), QByteArray("sentinel"));
        if (!missing) {
            QFile shared(source); QVERIFY(shared.open(QIODevice::ReadOnly));
            QCOMPARE(shared.readAll(), QByteArray("sentinel"));
        }
    }
    QVERIFY(QDir().mkdir(source));
    auto rejected = seedOrdinaryRuntime(package, directory.filePath("temp"));
    QVERIFY(!rejected.runtime);
    QVERIFY(!rejected.reason.isEmpty());
    QVERIFY(QFileInfo(source).isDir());
#endif
}

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

void DiagnosticActivationTest::savedChineseLanguageIsAppliedBeforeDiagnosticStartup() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings saved = AppSettings::defaults();
    saved.setLanguage(QStringLiteral("zh-CN"));
    QVERIFY(AppSettingsStore(directory.filePath("airplay-settings.json")).save(saved).success);

    int settingsLoadCount = 0;
    const StartupSettings startupSettings = loadStartupSettings(
        directory.path(), [&settingsLoadCount](const QString &settingsPath) {
            ++settingsLoadCount;
            return AppSettingsStore(settingsPath).loadOrDefaults();
        });
    QCOMPARE(startupSettings.settingsPath, directory.filePath("airplay-settings.json"));
    QVERIFY(startupSettings.settingsLoaded);
    QCOMPARE(settingsLoadCount, 1);
    QCOMPARE(startupSettings.settings.language(), QStringLiteral("zh-CN"));

    DiagnosticLifecycle diagnostics(QCoreApplication::instance());
    LanguageManager languageManager(QCoreApplication::instance(), nullptr,
                                    [] { return std::make_unique<QTranslator>(); },
                                    [](QTranslator *, const QString &) { return true; });
    const std::optional<TranslationLoadFailure> failure =
        applyStartupLanguage(languageManager, startupSettings.settings);

    QVERIFY(!failure.has_value());
    QCOMPARE(languageManager.selection(), QStringLiteral("zh-CN"));
    QCOMPARE(languageManager.effectiveLanguage(), QStringLiteral("zh-CN"));
    DiagnosticLifecycleStart start;
    start.applicationDirectory = directory.path();
    start.activation = {true, DiagnosticActivationSource::CommandArgument};
    const auto result = diagnostics.start(std::move(start));
    QVERIFY(result.creationError.isEmpty());
    QVERIFY(diagnostics.loggingActive());
}

void DiagnosticActivationTest::missingStartupCatalogFallsBackToEnglishAndRecordsOneDiagnosticEvent() {
    class CapturingSink final : public DiagnosticLogSink {
    public:
        void record(DiagnosticEvent event) override { events.append(std::move(event)); }
        QList<DiagnosticEvent> events;
    } sink;
    AppSettings settings = AppSettings::defaults();
    settings.setLanguage(QStringLiteral("zh-CN"));
    LanguageManager languageManager(QCoreApplication::instance(), nullptr,
                                    [] { return std::make_unique<QTranslator>(); },
                                    [](QTranslator *, const QString &) { return false; });

    const std::optional<TranslationLoadFailure> failure = applyStartupLanguage(languageManager, settings);

    QVERIFY(failure.has_value());
    QCOMPARE(languageManager.effectiveLanguage(), QStringLiteral("en"));
    QCOMPARE(QCoreApplication::translate("Startup", "Diagnostic logging unavailable"),
             QStringLiteral("Diagnostic logging unavailable"));
    languageManager.setDiagnosticSink(&sink);
    sink.record(*startupLanguageFailureEvent(failure));
    QCOMPARE(sink.events.size(), 1);
    QCOMPARE(sink.events.constFirst().name, QStringLiteral("translation_load_failed"));
    QCOMPARE(sink.events.constFirst().severity, DiagnosticSeverity::Warning);
    QVERIFY(sink.events.constFirst().flushImmediately);
    QCOMPARE(sink.events.constFirst().fields.value(QStringLiteral("language_selection")),
             QStringLiteral("zh-CN"));
    QCOMPARE(sink.events.constFirst().fields.value(QStringLiteral("effective_language")),
             QStringLiteral("en"));
    QCOMPARE(sink.events.constFirst().fields.value(QStringLiteral("catalog_id")),
             QStringLiteral("airplay_zh_CN"));
    QCOMPARE(sink.events.constFirst().fields.value(QStringLiteral("phase")),
             QStringLiteral("startup"));
    QCOMPARE(sink.events.constFirst().fields.value(QStringLiteral("result")),
             QStringLiteral("fallback"));
}

void DiagnosticActivationTest::startupTranslationFailureSurvivesPrivacyFiltering() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    options.activationSource = QStringLiteral("command_argument");
    auto created = DiagnosticSession::create(std::move(options));
    QVERIFY2(created.session != nullptr, qPrintable(created.error));

    created.session->record(*startupLanguageFailureEvent(
        TranslationLoadFailure{QStringLiteral("zh-CN"),
                               QStringLiteral(":/i18n/airplay_zh_CN.qm")}));

    QFile log(created.session->filePath());
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QByteArray output = log.readAll();
    QCOMPARE(output.count("translation_load_failed"), 1);
    QVERIFY(output.contains("WARN startup translation_load_failed"));
    QVERIFY(output.contains("language_selection=zh-CN"));
    QVERIFY(output.contains("effective_language=en"));
    QVERIFY(output.contains("catalog_id=airplay_zh_CN"));
    QVERIFY(output.contains("phase=startup"));
    QVERIFY(output.contains("result=fallback"));
    QVERIFY(!output.contains("resource_path"));
    QVERIFY(!output.contains(":/i18n/"));
}

void DiagnosticActivationTest::startupTranslationFailureRedactsUnsupportedSelection() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(std::move(options));
    QVERIFY2(created.session != nullptr, qPrintable(created.error));

    created.session->record(*startupLanguageFailureEvent(
        TranslationLoadFailure{QStringLiteral("Private language value"),
                               QStringLiteral("C:/Users/Private/catalog.qm")}));

    QFile log(created.session->filePath());
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QByteArray output = log.readAll();
    QVERIFY(output.contains("language_selection=unsupported"));
    QVERIFY(!output.contains("Private"));
    QVERIFY(!output.contains("catalog.qm"));
}

void DiagnosticActivationTest::startupSettingsSnapshotSurvivesPrivacyFiltering_data() {
    QTest::addColumn<bool>("saved");
    QTest::addColumn<QString>("selection");
    QTest::addColumn<bool>("catalogAvailable");
    QTest::addColumn<QString>("expectedSelection");
    QTest::addColumn<QString>("expectedEffective");
    QTest::newRow("defaults") << false << "system" << true << "system" << "en";
    QTest::newRow("saved Chinese") << true << "zh-CN" << true << "zh-CN" << "zh-CN";
    QTest::newRow("saved Chinese fallback") << true << "zh-CN" << false << "zh-CN" << "en";
    QTest::newRow("saved unsupported selection")
        << true << "Private language value" << true << "unsupported" << "en";
}

void DiagnosticActivationTest::startupSettingsSnapshotSurvivesPrivacyFiltering() {
    QFETCH(bool, saved);
    QFETCH(QString, selection);
    QFETCH(bool, catalogAvailable);
    QFETCH(QString, expectedSelection);
    QFETCH(QString, expectedEffective);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    if (saved) {
        AppSettings settings = AppSettings::defaults();
        settings.setLanguage(selection);
        settings.setAspectRatioLock(false);
        settings.setVideoFitMode(false);
        settings.setToolbarHoverReveal(false);
        settings.setReceiverName(QStringLiteral("Private receiver"));
        settings.setRecordingOutputDirectory(QStringLiteral("C:/Users/Private/recordings"));
        QVERIFY(AppSettingsStore(directory.filePath("airplay-settings.json")).save(settings).success);
    }
    const StartupSettings settings = loadStartupSettings(directory.path());
    LanguageManager manager(QCoreApplication::instance(), nullptr,
                            [] { return std::make_unique<QTranslator>(); },
                            [catalogAvailable](QTranslator *translator, const QString &resourcePath) {
        return catalogAvailable && translator->load(resourcePath);
    });
    manager.apply(settings.settings.language(), QLocale("en-US"));
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(std::move(options));
    QVERIFY2(created.session != nullptr, qPrintable(created.error));

    recordStartupSettings(created.session.get(), settings, manager);

    QFile log(created.session->filePath());
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QByteArray output = log.readAll();
    QVERIFY(output.contains(saved ? "INFO startup settings_loaded" : "INFO startup settings_defaulted"));
    QVERIFY(output.contains(saved ? "aspect_lock=no" : "aspect_lock=yes"));
    QVERIFY(output.contains(saved ? "video_fit=no" : "video_fit=yes"));
    QVERIFY(output.contains(saved ? "hover_reveal=no" : "hover_reveal=yes"));
    QVERIFY(output.contains("language_selection=" + expectedSelection.toUtf8()));
    QVERIFY(output.contains("effective_language=" + expectedEffective.toUtf8()));
    QVERIFY(!output.contains("Private"));
    QVERIFY(!output.contains("recordings"));
}

void DiagnosticActivationTest::diagnosticLoggingBodiesTranslateWithoutChangingRawDetails() {
    StartupTranslator translator;
    const InstalledTranslator installedTranslator(&translator);

    QCOMPARE(diagnosticLoggingUnavailableMessage(QStringLiteral("access denied")),
             QString::fromUtf8(u8"无法启动诊断日志：access denied"));
    QCOMPARE(diagnosticLoggingStoppedMessage(QStringLiteral("disk full")),
             QString::fromUtf8(u8"诊断日志已停止：disk full"));
    const QString mainError = QString::fromUtf8(u8"缺少文件：C:/应用/运行.dll\n请重新解压。");
    const QString detail = QString::fromUtf8(u8"磁盘已满：C:/日志/会话.log\n原始错误");
    QCOMPARE(startupFailureMessage(mainError, std::nullopt), mainError);
    QCOMPARE(startupFailureMessage(mainError, DiagnosticFailure{DiagnosticFailureKind::Creation, detail}),
             mainError + "\n\n" + QString::fromUtf8(u8"无法启动诊断日志：") + detail);
    QCOMPARE(startupFailureMessage(mainError, DiagnosticFailure{DiagnosticFailureKind::Write, detail}),
             mainError + "\n\n" + QString::fromUtf8(u8"诊断日志可能不完整：") + detail);
    QCOMPARE(startupText("Diagnostic log could not be fully saved: %1").arg(detail),
             QString::fromUtf8(u8"诊断日志未能完整保存：") + detail);
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
                                         QStringLiteral("character_count"),
                                         QStringLiteral("has_non_ascii"),
                                         QStringLiteral("result")}));
    QCOMPARE(fields.value(QStringLiteral("result")), QStringLiteral("no"));
    QCOMPARE(fields.value(QStringLiteral("has_non_ascii")), QStringLiteral("yes"));
    QCOMPARE(fields.value(QStringLiteral("ansi_code_page")), QStringLiteral("936"));
    QCOMPARE(fields.value(QStringLiteral("character_count")), QStringLiteral("42"));
}

void DiagnosticActivationTest::runtimePathCharacterCountSurvivesPrivacyFiltering() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DiagnosticSessionOptions options;
    options.applicationDirectory = directory.path();
    auto created = DiagnosticSession::create(std::move(options));
    QVERIFY2(created.session != nullptr, qPrintable(created.error));

    recordStartup(created.session.get(), QStringLiteral("runtime_path_compatibility"),
                  runtimePathCompatibilityFields({false, true, 936, 42}), true);

    QFile log(created.session->filePath());
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QByteArray output = log.readAll();
    QVERIFY(output.contains("character_count=42"));
    QVERIFY(output.contains("ansi_code_page=936"));
    QVERIFY(output.contains("has_non_ascii=yes"));
    QVERIFY(!output.contains("path_length"));
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

QTEST_GUILESS_MAIN(DiagnosticActivationTest)
#include "DiagnosticActivationTest.moc"

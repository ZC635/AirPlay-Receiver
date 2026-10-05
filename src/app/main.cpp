#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QTextStream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <optional>

#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/BuildIdentity.h"
#include "app/DiagnosticLifecycle.h"
#include "app/LanguageManager.h"
#include "app/MainWindow.h"
#include "app/RecordingStartupCleanup.h"
#include "backend/AirPlayReceiver.h"
#include "backend/UxPlayReceiver.h"
#include "diagnostics/DiagnosticLogSink.h"
#include "platform/DependencyDiagnostics.h"
#include "platform/WindowsHotkeyService.h"

struct StandaloneRuntimeDecision {
    bool emitManifestEntries = false;
    QString result;
};

struct RuntimePathStartupDecision {
    bool continueApplication = true;
    QString title;
    QString message;
    QString abortReason;
};

struct GStreamerPluginStartupDecision {
    bool continueApplication = true;
    QString title;
    QString message;
    QString abortReason;
    QString reason = QStringLiteral("ready");
};

struct StartupSettings {
    QString settingsPath;
    bool settingsLoaded = false;
    AppSettings settings = AppSettings::defaults();
};

struct TranslationLoadFailure {
    QString requestedLanguage;
    QString resourcePath;
};

QString startupText(const char *sourceText) {
    return QCoreApplication::translate("Startup", sourceText);
}

QString diagnosticLoggingUnavailableMessage(const QString &error) {
    if (error.isEmpty()) {
        return startupText(QT_TRANSLATE_NOOP(
            "Startup", "Diagnostic logging could not be started."));
    }
    return startupText(QT_TRANSLATE_NOOP(
        "Startup", "Diagnostic logging could not be started: %1")).arg(error);
}

QString diagnosticLoggingStoppedMessage(const QString &error) {
    if (error.isEmpty()) {
        return startupText(QT_TRANSLATE_NOOP("Startup", "Diagnostic logging stopped."));
    }
    return startupText(QT_TRANSLATE_NOOP(
        "Startup", "Diagnostic logging stopped: %1")).arg(error);
}

void showDiagnosticLoggingStoppedWarning(const QString &error) {
    QMessageBox::warning(nullptr,
                         startupText(QT_TRANSLATE_NOOP(
                             "Startup", "Diagnostic logging stopped")),
                         diagnosticLoggingStoppedMessage(error));
}

StandaloneRuntimeDecision diagnosticStandaloneRuntimeDecision(bool shouldCheck) {
    return {shouldCheck, shouldCheck ? QStringLiteral("checked") : QStringLiteral("skipped")};
}

bool runPortableGStreamerStartupSequence(
    bool shouldCheckManifest,
    const std::function<bool()> &verifyManifest,
    const std::function<void()> &configureGStreamerEnvironment,
    const std::function<bool()> &probeCorePlugins) {
    if (shouldCheckManifest && !verifyManifest()) {
        return false;
    }
    configureGStreamerEnvironment();
    return probeCorePlugins();
}

RuntimePathStartupDecision runtimePathStartupDecision(
    const RuntimePathCompatibility &compatibility) {
    if (compatibility.compatible) {
        return {};
    }

    return {false,
            startupText(QT_TRANSLATE_NOOP("Startup", "Unsupported application path")),
            startupText(QT_TRANSLATE_NOOP(
                "Startup", "The current application folder uses characters unsupported by the current "
                "Windows system language. Bundled GStreamer plugins cannot load from this "
                "location.\n\nMove the entire extracted application folder to a short path "
                "containing only English letters, numbers, spaces, hyphens, and underscores "
                "(for example, C:\\AirPlay), then restart the application.")),
            QStringLiteral("unsupported_application_path")};
}

QMap<QString, QString> runtimePathCompatibilityFields(
    const RuntimePathCompatibility &compatibility) {
    return {{QStringLiteral("result"), compatibility.compatible ? QStringLiteral("yes") : QStringLiteral("no")},
            {QStringLiteral("has_non_ascii"), compatibility.hasNonAscii ? QStringLiteral("yes") : QStringLiteral("no")},
            {QStringLiteral("ansi_code_page"), QString::number(compatibility.ansiCodePage)},
            {QStringLiteral("character_count"), QString::number(compatibility.pathLength)}};
}

GStreamerPluginStartupDecision gstreamerPluginStartupDecision(
    const GStreamerPluginReadiness &readiness, bool hasNonAscii) {
    if (readiness.ready) return {};
    const QString missing = readiness.missingPlugins.isEmpty()
        ? QStringLiteral("GStreamer initialization")
        : readiness.missingPlugins.join(QStringLiteral(", "));
    if (hasNonAscii) {
        return {false,
                startupText(QT_TRANSLATE_NOOP("Startup", "GStreamer plugins unavailable")),
                startupText(QT_TRANSLATE_NOOP("Startup", "Required GStreamer plugins could not be loaded from the current application folder:\n\n%1\n\nMove the entire extracted folder to a short path containing only English letters, numbers, spaces, hyphens, and underscores, for example C:\\AirPlay, then restart the application.")).arg(missing),
                QStringLiteral("gstreamer_plugin_path_load_failure"),
                QStringLiteral("path_load_failure")};
    }
    return {false,
            startupText(QT_TRANSLATE_NOOP("Startup", "GStreamer plugins unavailable")),
            startupText(QT_TRANSLATE_NOOP("Startup", "Required GStreamer plugins could not be loaded:\n\n%1\n\nRe-extract the portable package. If the problem persists, restart with diagnostic logging and report the generated log.")).arg(missing),
            QStringLiteral("gstreamer_plugin_load_failure"),
            QStringLiteral("plugin_load_failure")};
}

QMap<QString, QString> gstreamerPluginReadinessFields(
    const GStreamerPluginReadiness &readiness, bool hasNonAscii,
    const QString &reason) {
    return {{QStringLiteral("result"), readiness.ready ? QStringLiteral("yes") : QStringLiteral("no")},
            {QStringLiteral("missing_count"), QString::number(readiness.missingPlugins.size())},
            {QStringLiteral("missing_plugins"), readiness.missingPlugins.join(QLatin1Char(','))},
            {QStringLiteral("has_non_ascii"), hasNonAscii ? QStringLiteral("yes") : QStringLiteral("no")},
            {QStringLiteral("reason"), reason}};
}

namespace {

void recordStartup(DiagnosticLogSink *sink, QString name,
                   QMap<QString, QString> fields = {}, bool flush = false) {
    sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("startup"),
                                    std::move(name), std::move(fields), flush));
}

QString diagnosticLanguageSelection(const QString &selection) {
    const QString normalized = selection.trimmed();
    if (normalized.isEmpty() || normalized == QStringLiteral("system")) {
        return QStringLiteral("system");
    }
    if (normalized == QStringLiteral("en") || normalized == QStringLiteral("zh-CN")) {
        return normalized;
    }
    return QStringLiteral("unsupported");
}

bool processIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                               sizeof(elevation), &bytes) &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool settingsFileContainsObject(const QString &settingsPath) {
    QFile file(settingsPath);
    return file.open(QIODevice::ReadOnly) &&
        QJsonDocument::fromJson(file.readAll()).isObject();
}

StartupSettings loadStartupSettings(
    const QString &applicationDirectory,
    const std::function<AppSettings(const QString &)> &loadSettings = {}) {
    StartupSettings result;
    result.settingsPath = applicationDirectory + QStringLiteral("/airplay-settings.json");
    result.settingsLoaded = settingsFileContainsObject(result.settingsPath);
    result.settings = loadSettings ? loadSettings(result.settingsPath)
                                   : AppSettingsStore(result.settingsPath).loadOrDefaults();
    return result;
}

std::optional<TranslationLoadFailure> applyStartupLanguage(
    LanguageManager &languageManager, const AppSettings &settings) {
    std::optional<TranslationLoadFailure> failure;
    const QMetaObject::Connection connection = QObject::connect(
        &languageManager, &LanguageManager::translationLoadFailed, &languageManager,
        [&failure](const QString &selection, const QString &resourcePath) {
            failure = TranslationLoadFailure{selection, resourcePath};
        });
    languageManager.apply(settings.language());
    QObject::disconnect(connection);
    return failure;
}

std::optional<DiagnosticEvent> startupLanguageFailureEvent(
    const std::optional<TranslationLoadFailure> &failure) {
    if (!failure.has_value()) {
        return std::nullopt;
    }
    return makeDiagnosticEvent(DiagnosticSeverity::Warning, QStringLiteral("startup"),
        QStringLiteral("translation_load_failed"),
        {{QStringLiteral("language_selection"), diagnosticLanguageSelection(failure->requestedLanguage)},
         {QStringLiteral("effective_language"), QStringLiteral("en")},
         {QStringLiteral("catalog_id"), QStringLiteral("airplay_zh_CN")},
         {QStringLiteral("phase"), QStringLiteral("startup")},
         {QStringLiteral("result"), QStringLiteral("fallback")}}, true);
}

void recordStartupSettings(DiagnosticLogSink *sink, const StartupSettings &settings,
                           const LanguageManager &languageManager) {
    recordStartup(sink, settings.settingsLoaded ? QStringLiteral("settings_loaded")
                                               : QStringLiteral("settings_defaulted"),
                  {{QStringLiteral("aspect_lock"), settings.settings.aspectRatioLock()
                        ? QStringLiteral("yes") : QStringLiteral("no")},
                   {QStringLiteral("video_fit"), settings.settings.videoFitMode()
                        ? QStringLiteral("yes") : QStringLiteral("no")},
                   {QStringLiteral("hover_reveal"), settings.settings.toolbarHoverReveal()
                        ? QStringLiteral("yes") : QStringLiteral("no")},
                   {QStringLiteral("language_selection"),
                        diagnosticLanguageSelection(settings.settings.language())},
                   {QStringLiteral("effective_language"), languageManager.effectiveLanguage()}}, true);
}

} // namespace

int main(int argc, char *argv[]) {
    bool verifyRecordingRuntime = false;
    for (int index = 1; index < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == "--verify-recording-runtime") {
            verifyRecordingRuntime = true;
            break;
        }
    }
    if (verifyRecordingRuntime) {
        QCoreApplication app(argc, argv);
#if AIRPLAY_WITH_UXPLAY
        const RuntimePathCompatibility runtimePathCompatibility =
            DependencyDiagnostics::checkRuntimePathCompatibility(
                QCoreApplication::applicationDirPath());
        const RuntimePathStartupDecision runtimePathDecision =
            runtimePathStartupDecision(runtimePathCompatibility);
        if (!runtimePathDecision.continueApplication) {
            QTextStream(stderr)
                << "Portable runtime verification failed: unsupported application path. "
                   "Move the entire extracted application folder to a short path such as "
                   "C:\\AirPlay."
                << Qt::endl;
            return 3;
        }
        DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
            QCoreApplication::applicationDirPath());
        const GStreamerPluginReadiness pluginReadiness =
            DependencyDiagnostics::checkGStreamerPluginReadiness();
        if (!pluginReadiness.ready) {
            QTextStream(stderr)
                << "Portable runtime verification failed; missing GStreamer plugins: "
                << (pluginReadiness.missingPlugins.isEmpty()
                        ? QStringLiteral("initialization failed")
                        : pluginReadiness.missingPlugins.join(QStringLiteral(", ")))
                << Qt::endl;
            return 4;
        }
        const RecordingCapabilityDiagnostics diagnostics =
            DependencyDiagnostics::checkRecordingCapabilities(true);
        if (diagnostics.canRecord) {
            QTextStream(stdout)
                << "Recording runtime verified; selected encoder: "
                << diagnostics.selectedEncoder
                << "; required encoders: mfh264enc, openh264enc" << Qt::endl;
            return 0;
        }
        QTextStream(stderr)
            << "Recording runtime verification failed; missing factories: "
            << diagnostics.missingFactories.join(", ") << Qt::endl;
        return 2;
#else
        QTextStream(stderr) << "Recording runtime support is not built" << Qt::endl;
        return 2;
#endif
    }

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("AirPlay Receiver"));
    QCoreApplication::setApplicationVersion(QString::fromUtf16(AirPlayBuildIdentity::version));

    const StartupSettings startupSettings = loadStartupSettings(
        QCoreApplication::applicationDirPath());
    MainWindow *diagnosticWindow = nullptr;
    DiagnosticLifecycle diagnostics(&app);
    LanguageManager languageManager(&app);
    const std::optional<TranslationLoadFailure> translationLoadFailure =
        applyStartupLanguage(languageManager, startupSettings.settings);

    const DiagnosticActivation activation = DiagnosticActivation::parse(
        QCoreApplication::arguments(), qgetenv("AIRPLAY_DEBUG_LOG"));
    DiagnosticLifecycleStart diagnosticStart;
    diagnosticStart.applicationDirectory = QCoreApplication::applicationDirPath();
    diagnosticStart.activation = activation;
    diagnosticStart.beforeChildGateEvent = startupLanguageFailureEvent(translationLoadFailure);
    diagnosticStart.reportWriteFailure = [&diagnosticWindow](QString error) {
        if (diagnosticWindow != nullptr) {
            diagnosticWindow->handleDiagnosticWriteFailure(std::move(error));
        }
    };
    const DiagnosticLifecycleStartupResult diagnosticResult = diagnostics.start(std::move(diagnosticStart));
    if (diagnosticResult.childGate == DiagnosticChildGateResult::ExitChild) {
        return 1;
    }
    DiagnosticLogSink *sink = diagnostics.sink();
    languageManager.setDiagnosticSink(sink);
    if (!diagnosticResult.creationError.isEmpty()) {
        QMessageBox::warning(nullptr,
                             startupText(QT_TRANSLATE_NOOP(
                                 "Startup", "Diagnostic logging unavailable")),
                             diagnosticLoggingUnavailableMessage(diagnosticResult.creationError));
    }
    recordStartup(sink, QStringLiteral("session_activation"),
                  {{QStringLiteral("activation_source"), activation.sourceName()}}, true);
    recordStartup(sink, QStringLiteral("application_identity"),
                  {{QStringLiteral("build_id"), QString::fromUtf16(AirPlayBuildIdentity::buildId)},
                   {QStringLiteral("elevated"), processIsElevated() ? QStringLiteral("yes") : QStringLiteral("no")},
                   {QStringLiteral("executable_name"), QFileInfo(QCoreApplication::applicationFilePath()).fileName()},
                   {QStringLiteral("version"), QString::fromUtf16(AirPlayBuildIdentity::version)}}, true);

#if AIRPLAY_WITH_UXPLAY
    const RuntimePathCompatibility runtimePathCompatibility =
        DependencyDiagnostics::checkRuntimePathCompatibility(
            QCoreApplication::applicationDirPath());
    recordStartup(sink, QStringLiteral("runtime_path_compatibility"),
                  runtimePathCompatibilityFields(runtimePathCompatibility), true);
    const RuntimePathStartupDecision runtimePathDecision =
        runtimePathStartupDecision(runtimePathCompatibility);
    if (!runtimePathDecision.continueApplication) {
        QMessageBox::critical(nullptr, runtimePathDecision.title, runtimePathDecision.message);
        diagnostics.abortStartup(runtimePathDecision.abortReason);
        return 1;
    }
#endif

    diagnostics.collectEnvironmentAndStartMonitor();

#if AIRPLAY_WITH_UXPLAY
    const StandaloneRuntimeDecision runtimeDecision = diagnosticStandaloneRuntimeDecision(
        DependencyDiagnostics::shouldCheckStandaloneRuntime());
    if (!runtimeDecision.emitManifestEntries) {
        recordStartup(sink, QStringLiteral("standalone_runtime_check"),
                      {{QStringLiteral("result"), runtimeDecision.result}}, true);
    }

    const bool gstreamerStartupReady = runPortableGStreamerStartupSequence(
        runtimeDecision.emitManifestEntries,
        [&] {
            const StandaloneRuntimeSnapshot runtimeSnapshot =
                DependencyDiagnostics::standaloneRuntimeSnapshot(
                    QCoreApplication::applicationDirPath());
            for (const QString &relativeName : runtimeSnapshot.relativePaths) {
                const bool present = !runtimeSnapshot.missingRelativePaths.contains(relativeName);
                recordStartup(sink, QStringLiteral("runtime_manifest_entry"),
                              {{QStringLiteral("relative_name"), relativeName},
                               {QStringLiteral("result"), present ? QStringLiteral("present") : QStringLiteral("missing")}});
            }
            if (runtimeSnapshot.complete) {
                return true;
            }
            QMessageBox::critical(
                nullptr,
                startupText(QT_TRANSLATE_NOOP(
                    "Startup", "AirPlay Receiver dependencies missing")),
                startupText(QT_TRANSLATE_NOOP(
                    "Startup", "This standalone build is missing required runtime files:\n\n%1\n\nRun scripts\\build.ps1 -Deploy, then launch airplay_receiver.exe again."))
                    .arg(runtimeSnapshot.missingRelativePaths.join('\n')));
            diagnostics.abortStartup(QStringLiteral("missing_runtime"));
            return false;
        },
        [&] {
            const bool gstreamerEnvironmentConfigured =
                DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
                    QCoreApplication::applicationDirPath());
            recordStartup(sink, QStringLiteral("gstreamer_package_environment"),
                          {{QStringLiteral("result"), gstreamerEnvironmentConfigured ? QStringLiteral("yes") : QStringLiteral("no")}},
                          true);
        },
        [&] {
            const GStreamerPluginReadiness pluginReadiness =
                DependencyDiagnostics::checkGStreamerPluginReadiness();
            const GStreamerPluginStartupDecision pluginDecision =
                gstreamerPluginStartupDecision(pluginReadiness,
                                                runtimePathCompatibility.hasNonAscii);
            recordStartup(sink, "gstreamer_plugin_readiness",
                          gstreamerPluginReadinessFields(pluginReadiness,
                                                          runtimePathCompatibility.hasNonAscii,
                                                          pluginDecision.reason), true);
            if (pluginDecision.continueApplication) {
                return true;
            }
            QMessageBox::critical(nullptr, pluginDecision.title, pluginDecision.message);
            diagnostics.abortStartup(pluginDecision.abortReason);
            return false;
        });
    if (!gstreamerStartupReady) {
        return 1;
    }

    const RecordingCapabilityDiagnostics recordingDiagnostics =
        DependencyDiagnostics::checkRecordingCapabilities(false);
    recordStartup(sink, QStringLiteral("recording_capability"),
                  {{QStringLiteral("missing_count"), QString::number(recordingDiagnostics.missingFactories.size())},
                   {QStringLiteral("result"), recordingDiagnostics.canRecord ? QStringLiteral("yes") : QStringLiteral("no")},
                   {QStringLiteral("selected_encoder"), recordingDiagnostics.selectedEncoder}}, true);
    if (!recordingDiagnostics.canRecord) {
        qWarning().noquote()
            << "Recording capability unavailable; playback remains enabled. Missing factories:"
            << recordingDiagnostics.missingFactories.join(", ");
    }
#endif

    WindowsHotkeyService hotkeys;
    const QString &settingsPath = startupSettings.settingsPath;
    const AppSettings &settings = startupSettings.settings;
    recordStartupSettings(sink, startupSettings, languageManager);
    const QStringList cleanupResult = cleanupRecordingDirectoryAtStartup(settings);
    recordStartup(sink, QStringLiteral("recording_startup_cleanup"),
                  {{QStringLiteral("count"), QString::number(cleanupResult.size())},
                   {QStringLiteral("result"), QStringLiteral("completed")}}, true);

#if AIRPLAY_WITH_UXPLAY
    UxPlayReceiverConfig config;
    config.serverName = settings.receiverName();
    config.videoQuality = settings.videoQuality();
    config.videoSink = "appsink";
    config.audioSink = "wasapisink";
    config.diagnosticSink = sink;
    UxPlayReceiver receiver(config);
    bool startupCompletedRecorded = false;
    QObject::connect(&receiver, &AirPlayReceiver::stateChanged, &app,
                     [&sink, &startupCompletedRecorded](ReceiverState state) {
        if (startupCompletedRecorded ||
            (state != ReceiverState::Discoverable && state != ReceiverState::Error)) {
            return;
        }
        startupCompletedRecorded = true;
        recordStartup(sink, QStringLiteral("startup_completed"),
                      {{QStringLiteral("result"), state == ReceiverState::Discoverable
                          ? QStringLiteral("yes") : QStringLiteral("no")}}, true);
    });
    MainWindowRuntimeServices runtimeServices;
    runtimeServices.diagnosticSink = sink;
    runtimeServices.languageManager = &languageManager;
    runtimeServices.diagnosticLoggingActive = diagnostics.loggingActive();
    runtimeServices.quitApplication = [&app] { app.quit(); };
    MainWindow window(settings, &hotkeys, &receiver, settingsPath, nullptr, nullptr,
                      runtimeServices);
    recordStartup(sink, QStringLiteral("window_constructed"), {}, true);
    QObject::connect(&window, &MainWindow::diagnosticLoggingStopped, &app,
                     [](const QString &error) {
        showDiagnosticLoggingStoppedWarning(error);
    });
    diagnosticWindow = &window;
    recordStartup(sink, QStringLiteral("receiver_start_requested"), {}, true);
    receiver.start();
    window.show();
    const int exitCode = app.exec();
    diagnostics.exitNormally([&receiver] { receiver.stop(); });
    return exitCode;
#else
    MainWindowRuntimeServices runtimeServices;
    runtimeServices.diagnosticSink = sink;
    runtimeServices.languageManager = &languageManager;
    runtimeServices.diagnosticLoggingActive = diagnostics.loggingActive();
    runtimeServices.quitApplication = [&app] { app.quit(); };
    MainWindow window(settings, &hotkeys, nullptr, settingsPath, nullptr, nullptr,
                      runtimeServices);
    recordStartup(sink, QStringLiteral("window_constructed"), {}, true);
    QObject::connect(&window, &MainWindow::diagnosticLoggingStopped, &app,
                     [](const QString &error) {
        showDiagnosticLoggingStoppedWarning(error);
    });
    diagnosticWindow = &window;
    recordStartup(sink, QStringLiteral("receiver_start_requested"),
                  {{QStringLiteral("result"), QStringLiteral("not_built")}}, true);
    window.show();
    const int exitCode = app.exec();
    diagnostics.exitNormally({});
    return exitCode;
#endif
}

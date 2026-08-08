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

#include <memory>
#include <optional>

#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/DiagnosticActivation.h"
#include "app/MainWindow.h"
#include "app/RecordingStartupCleanup.h"
#include "backend/AirPlayReceiver.h"
#include "backend/UxPlayReceiver.h"
#include "diagnostics/DiagnosticLogSink.h"
#include "diagnostics/DiagnosticSession.h"
#include "diagnostics/QtDiagnosticMessageBridge.h"
#include "platform/DependencyDiagnostics.h"
#include "platform/WindowsHotkeyService.h"

struct DiagnosticStartupDecision {
    bool loggingActive = false;
    bool continueApplication = true;
    QString userError;
};

DiagnosticStartupDecision diagnosticStartupDecision(
    const DiagnosticActivation &activation, bool sessionCreated,
    const QString &creationError) {
    if (!activation.enabled) {
        return {};
    }
    return {sessionCreated, true, sessionCreated ? QString() : creationError};
}

namespace {

void recordStartup(DiagnosticLogSink *sink, QString name,
                   QMap<QString, QString> fields = {}, bool flush = false) {
    sink->record(makeDiagnosticEvent(DiagnosticSeverity::Info, QStringLiteral("startup"),
                                    std::move(name), std::move(fields), flush));
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

void closeDiagnosticSession(DiagnosticLogSink *sink,
                            std::optional<QtDiagnosticMessageBridge> &qtBridge,
                            std::unique_ptr<DiagnosticSession> &session,
                            AirPlayReceiver *receiver) {
    recordStartup(sink, QStringLiteral("shutdown_started"), {}, true);
    if (receiver != nullptr) {
        receiver->stop();
    }
    qtBridge.reset();
    if (session) {
        session->closeNormally();
    }
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
        DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
            QCoreApplication::applicationDirPath());
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
    QCoreApplication::setApplicationVersion(QStringLiteral(AIRPLAY_VERSION));

    const DiagnosticActivation activation = DiagnosticActivation::parse(
        QCoreApplication::arguments(), qgetenv("AIRPLAY_DEBUG_LOG"));
    std::unique_ptr<DiagnosticSession> session;
    DiagnosticLogSink *sink = &nullDiagnosticLogSink();
    QString creationError;
    if (activation.enabled) {
        DiagnosticSessionOptions options;
        options.applicationDirectory = QCoreApplication::applicationDirPath();
        options.activationSource = activation.sourceName();
        auto created = DiagnosticSession::create(std::move(options));
        creationError = created.error;
        if (created.session) {
            session = std::move(created.session);
            sink = session.get();
        }
    }
    const DiagnosticStartupDecision startupDecision = diagnosticStartupDecision(
        activation, session != nullptr, creationError);
    if (!startupDecision.userError.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Diagnostic logging unavailable"),
                             startupDecision.userError);
    }
    std::optional<QtDiagnosticMessageBridge> qtBridge;
    if (session) {
        qtBridge.emplace(sink);
    }

    recordStartup(sink, QStringLiteral("session_activation"),
                  {{QStringLiteral("activation_source"), activation.sourceName()}}, true);
    recordStartup(sink, QStringLiteral("application_identity"),
                  {{QStringLiteral("build_id"), QStringLiteral(AIRPLAY_BUILD_ID)},
                   {QStringLiteral("elevated"), processIsElevated() ? QStringLiteral("yes") : QStringLiteral("no")},
                   {QStringLiteral("executable_name"), QFileInfo(QCoreApplication::applicationFilePath()).fileName()},
                   {QStringLiteral("version"), QStringLiteral(AIRPLAY_VERSION)}}, true);

#if AIRPLAY_WITH_UXPLAY
    const bool gstreamerEnvironmentConfigured =
        DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
            QCoreApplication::applicationDirPath());
    recordStartup(sink, QStringLiteral("gstreamer_package_environment"),
                  {{QStringLiteral("result"), gstreamerEnvironmentConfigured ? QStringLiteral("yes") : QStringLiteral("no")}},
                  true);

    const bool checkStandaloneRuntime = DependencyDiagnostics::shouldCheckStandaloneRuntime();
    const StandaloneRuntimeSnapshot runtimeSnapshot =
        DependencyDiagnostics::standaloneRuntimeSnapshot(QCoreApplication::applicationDirPath());
    for (const QString &relativeName : runtimeSnapshot.relativePaths) {
        const bool present = !runtimeSnapshot.missingRelativePaths.contains(relativeName);
        recordStartup(sink, QStringLiteral("runtime_manifest_entry"),
                      {{QStringLiteral("relative_name"), relativeName},
                       {QStringLiteral("result"), present ? QStringLiteral("present") : QStringLiteral("missing")}});
    }
    if (checkStandaloneRuntime && !runtimeSnapshot.complete) {
        QMessageBox::critical(
            nullptr,
            QStringLiteral("AirPlay Receiver dependencies missing"),
            QString("This standalone build is missing required runtime files:\n\n%1\n\nRun scripts\\build.ps1 -Deploy, then launch airplay_receiver.exe again.")
                .arg(runtimeSnapshot.missingRelativePaths.join('\n')));
        closeDiagnosticSession(sink, qtBridge, session, nullptr);
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
    const QString settingsPath = QCoreApplication::applicationDirPath() + "/airplay-settings.json";
    const bool settingsLoaded = settingsFileContainsObject(settingsPath);
    const AppSettings settings = AppSettingsStore(settingsPath).loadOrDefaults();
    recordStartup(sink, settingsLoaded ? QStringLiteral("settings_loaded")
                                       : QStringLiteral("settings_defaulted"), {}, true);
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
    MainWindow window(settings, &hotkeys, &receiver, settingsPath);
    recordStartup(sink, QStringLiteral("window_constructed"), {}, true);
    window.setDiagnosticLoggingActive(startupDecision.loggingActive);
    if (session) {
        QObject::connect(session.get(), &DiagnosticSession::writeFailed, &window,
                         &MainWindow::handleDiagnosticWriteFailure, Qt::QueuedConnection);
    }
    QObject::connect(&window, &MainWindow::diagnosticLoggingStopped, &app,
                     [](const QString &error) {
        QMessageBox::warning(nullptr, QStringLiteral("Diagnostic logging stopped"), error);
    });
    recordStartup(sink, QStringLiteral("receiver_start_requested"), {}, true);
    receiver.start();
    window.show();
    const int exitCode = app.exec();
    closeDiagnosticSession(sink, qtBridge, session, &receiver);
    return exitCode;
#else
    MainWindow window(settings, &hotkeys, nullptr, settingsPath);
    recordStartup(sink, QStringLiteral("window_constructed"), {}, true);
    window.setDiagnosticLoggingActive(startupDecision.loggingActive);
    if (session) {
        QObject::connect(session.get(), &DiagnosticSession::writeFailed, &window,
                         &MainWindow::handleDiagnosticWriteFailure, Qt::QueuedConnection);
    }
    QObject::connect(&window, &MainWindow::diagnosticLoggingStopped, &app,
                     [](const QString &error) {
        QMessageBox::warning(nullptr, QStringLiteral("Diagnostic logging stopped"), error);
    });
    recordStartup(sink, QStringLiteral("receiver_start_requested"),
                  {{QStringLiteral("result"), QStringLiteral("not_built")}}, true);
    window.show();
    const int exitCode = app.exec();
    closeDiagnosticSession(sink, qtBridge, session, nullptr);
    return exitCode;
#endif
}

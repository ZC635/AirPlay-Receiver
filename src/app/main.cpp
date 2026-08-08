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
#include "app/DiagnosticActivation.h"
#include "app/MainWindow.h"
#include "app/RecordingStartupCleanup.h"
#include "backend/AirPlayReceiver.h"
#include "backend/UxPlayReceiver.h"
#include "diagnostics/DiagnosticLogSink.h"
#include "diagnostics/DiagnosticSession.h"
#include "diagnostics/QtDiagnosticMessageBridge.h"
#include "platform/DependencyDiagnostics.h"
#include "platform/EnvironmentDiagnostics.h"
#include "platform/NetworkDiagnosticsMonitor.h"
#include "platform/WindowsEnvironmentDiagnostics.h"
#include "platform/WindowsHotkeyService.h"

struct DiagnosticStartupDecision {
    bool loggingActive = false;
    bool continueApplication = true;
    QString userError;
};

struct DiagnosticShutdownDecision {
    bool recordStartupAborted = false;
    bool recordShutdownStarted = false;
    bool closeNormally = false;
};

struct StandaloneRuntimeDecision {
    bool emitManifestEntries = false;
    QString result;
};

DiagnosticStartupDecision diagnosticStartupDecision(
    const DiagnosticActivation &activation, bool sessionCreated,
    const QString &creationError) {
    if (!activation.enabled) {
        return {};
    }
    return {sessionCreated, true, sessionCreated ? QString() : creationError};
}

DiagnosticShutdownDecision diagnosticShutdownDecision(bool normalExit) {
    return { !normalExit, normalExit, normalExit };
}

StandaloneRuntimeDecision diagnosticStandaloneRuntimeDecision(bool shouldCheck) {
    return {shouldCheck, shouldCheck ? QStringLiteral("checked") : QStringLiteral("skipped")};
}

bool diagnosticLoggingActiveForSession(bool sessionCreated, bool sessionActive) {
    return sessionCreated && sessionActive;
}

class DiagnosticWriteFailureRelay {
public:
    void setHandler(std::function<void(QString)> handler) {
        handler_ = std::move(handler);
    }

    void deliver(QString error) const {
        if (handler_) {
            handler_(std::move(error));
        }
    }

private:
    std::function<void(QString)> handler_;
};

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
                            std::unique_ptr<NetworkDiagnosticsMonitor> &networkMonitor,
                            AirPlayReceiver *receiver) {
    const DiagnosticShutdownDecision decision = diagnosticShutdownDecision(true);
    if (decision.recordShutdownStarted) {
        recordStartup(sink, QStringLiteral("shutdown_started"), {}, true);
    }
    if (receiver != nullptr) {
        receiver->stop();
    }
    if (networkMonitor) {
        networkMonitor->stop();
        networkMonitor.reset();
    }
    qtBridge.reset();
    if (decision.closeNormally && session) {
        session->closeNormally();
    }
}

void abortDiagnosticSession(DiagnosticLogSink *sink,
                            std::optional<QtDiagnosticMessageBridge> &qtBridge,
                            std::unique_ptr<DiagnosticSession> &session,
                            std::unique_ptr<NetworkDiagnosticsMonitor> &networkMonitor) {
    const DiagnosticShutdownDecision decision = diagnosticShutdownDecision(false);
    if (decision.recordStartupAborted) {
        recordStartup(sink, QStringLiteral("startup_aborted"),
                      {{QStringLiteral("reason"), QStringLiteral("missing_runtime")}}, true);
    }
    if (networkMonitor) {
        networkMonitor->stop();
        networkMonitor.reset();
    }
    qtBridge.reset();
    session.reset();
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
    QCoreApplication::setApplicationVersion(QString::fromUtf16(AirPlayBuildIdentity::version));

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
    MainWindow *diagnosticWindow = nullptr;
    DiagnosticWriteFailureRelay writeFailureRelay;
    writeFailureRelay.setHandler([&diagnosticWindow](QString error) {
        if (diagnosticWindow != nullptr) {
            diagnosticWindow->handleDiagnosticWriteFailure(std::move(error));
        }
    });
    if (session) {
        QObject::connect(session.get(), &DiagnosticSession::writeFailed, &app,
                         [&writeFailureRelay](const QString &error) {
            writeFailureRelay.deliver(error);
        }, Qt::QueuedConnection);
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
                  {{QStringLiteral("build_id"), QString::fromUtf16(AirPlayBuildIdentity::buildId)},
                   {QStringLiteral("elevated"), processIsElevated() ? QStringLiteral("yes") : QStringLiteral("no")},
                   {QStringLiteral("executable_name"), QFileInfo(QCoreApplication::applicationFilePath()).fileName()},
                   {QStringLiteral("version"), QString::fromUtf16(AirPlayBuildIdentity::version)}}, true);

    std::unique_ptr<NetworkDiagnosticsMonitor> networkMonitor;
    if (shouldCollectEnvironmentDiagnostics(session && session->isActive())) {
        const EnvironmentSnapshot environmentSnapshot = EnvironmentDiagnostics::collect(
            windowsEnvironmentDiagnosticProviders(), 3000);
        for (const DiagnosticEvent &event : EnvironmentDiagnostics::events(environmentSnapshot)) {
            sink->record(event);
        }
        if (shouldCollectEnvironmentDiagnostics(session && session->isActive())) {
            networkMonitor = std::make_unique<NetworkDiagnosticsMonitor>(
                sink, windowsNetworkMonitorOperations(), environmentSnapshot.network);
            if (!networkMonitor->start()) {
                recordStartup(sink, QStringLiteral("network_monitor"),
                              {{QStringLiteral("result"), QStringLiteral("unavailable")}}, true);
                networkMonitor.reset();
            }
        }
    }

#if AIRPLAY_WITH_UXPLAY
    const bool gstreamerEnvironmentConfigured =
        DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
            QCoreApplication::applicationDirPath());
    recordStartup(sink, QStringLiteral("gstreamer_package_environment"),
                  {{QStringLiteral("result"), gstreamerEnvironmentConfigured ? QStringLiteral("yes") : QStringLiteral("no")}},
                  true);

    const StandaloneRuntimeDecision runtimeDecision = diagnosticStandaloneRuntimeDecision(
        DependencyDiagnostics::shouldCheckStandaloneRuntime());
    if (!runtimeDecision.emitManifestEntries) {
        recordStartup(sink, QStringLiteral("standalone_runtime_check"),
                      {{QStringLiteral("result"), runtimeDecision.result}}, true);
    } else {
        const StandaloneRuntimeSnapshot runtimeSnapshot =
            DependencyDiagnostics::standaloneRuntimeSnapshot(QCoreApplication::applicationDirPath());
        for (const QString &relativeName : runtimeSnapshot.relativePaths) {
            const bool present = !runtimeSnapshot.missingRelativePaths.contains(relativeName);
            recordStartup(sink, QStringLiteral("runtime_manifest_entry"),
                          {{QStringLiteral("relative_name"), relativeName},
                           {QStringLiteral("result"), present ? QStringLiteral("present") : QStringLiteral("missing")}});
        }
        if (!runtimeSnapshot.complete) {
            QMessageBox::critical(
                nullptr,
                QStringLiteral("AirPlay Receiver dependencies missing"),
                QString("This standalone build is missing required runtime files:\n\n%1\n\nRun scripts\\build.ps1 -Deploy, then launch airplay_receiver.exe again.")
                    .arg(runtimeSnapshot.missingRelativePaths.join('\n')));
            abortDiagnosticSession(sink, qtBridge, session, networkMonitor);
            return 1;
        }
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
    QObject::connect(&window, &MainWindow::diagnosticLoggingStopped, &app,
                     [](const QString &error) {
        QMessageBox::warning(nullptr, QStringLiteral("Diagnostic logging stopped"), error);
    });
    diagnosticWindow = &window;
    window.setDiagnosticLoggingActive(diagnosticLoggingActiveForSession(
        session != nullptr, session && session->isActive()));
    recordStartup(sink, QStringLiteral("receiver_start_requested"), {}, true);
    receiver.start();
    window.show();
    const int exitCode = app.exec();
    closeDiagnosticSession(sink, qtBridge, session, networkMonitor, &receiver);
    return exitCode;
#else
    MainWindow window(settings, &hotkeys, nullptr, settingsPath);
    recordStartup(sink, QStringLiteral("window_constructed"), {}, true);
    QObject::connect(&window, &MainWindow::diagnosticLoggingStopped, &app,
                     [](const QString &error) {
        QMessageBox::warning(nullptr, QStringLiteral("Diagnostic logging stopped"), error);
    });
    diagnosticWindow = &window;
    window.setDiagnosticLoggingActive(diagnosticLoggingActiveForSession(
        session != nullptr, session && session->isActive()));
    recordStartup(sink, QStringLiteral("receiver_start_requested"),
                  {{QStringLiteral("result"), QStringLiteral("not_built")}}, true);
    window.show();
    const int exitCode = app.exec();
    closeDiagnosticSession(sink, qtBridge, session, networkMonitor, nullptr);
    return exitCode;
#endif
}

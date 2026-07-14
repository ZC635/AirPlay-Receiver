#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QMessageBox>
#include <QTextStream>

#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/MainWindow.h"
#include "app/RecordingStartupCleanup.h"
#include "backend/UxPlayReceiver.h"
#include "platform/DependencyDiagnostics.h"
#include "platform/WindowsHotkeyService.h"

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

#if AIRPLAY_WITH_UXPLAY
    DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
        QCoreApplication::applicationDirPath());
    const QStringList missingRuntime = DependencyDiagnostics::shouldCheckStandaloneRuntime()
        ? DependencyDiagnostics::checkStandaloneRuntime(QCoreApplication::applicationDirPath())
        : QStringList{};
    if (!missingRuntime.isEmpty()) {
        QMessageBox::critical(
            nullptr,
            "AirPlay Receiver dependencies missing",
            QString("This standalone build is missing required runtime files:\n\n%1\n\nRun scripts\\build.ps1 -Deploy, then launch airplay_receiver.exe again.")
                .arg(missingRuntime.join('\n')));
        return 1;
    }
    const RecordingCapabilityDiagnostics recordingDiagnostics =
        DependencyDiagnostics::checkRecordingCapabilities(false);
    if (!recordingDiagnostics.canRecord) {
        qWarning().noquote()
            << "Recording capability unavailable; playback remains enabled. Missing factories:"
            << recordingDiagnostics.missingFactories.join(", ");
    }
#endif

    WindowsHotkeyService hotkeys;
    const QString settingsPath = QCoreApplication::applicationDirPath() + "/airplay-settings.json";
    const AppSettings settings = AppSettingsStore(settingsPath).loadOrDefaults();
    cleanupRecordingDirectoryAtStartup(settings);

#if AIRPLAY_WITH_UXPLAY
    UxPlayReceiverConfig config;
    config.serverName = settings.receiverName();
    config.videoQuality = settings.videoQuality();
    config.videoSink = "appsink";
    config.audioSink = "wasapisink";
    UxPlayReceiver receiver(config);
    QObject::connect(&app, &QApplication::aboutToQuit, &receiver, [&receiver] {
        receiver.stop();
    });
    MainWindow window(settings, &hotkeys, &receiver, settingsPath);
    receiver.start();
#else
    MainWindow window(settings, &hotkeys, nullptr, settingsPath);
#endif

    window.show();
    return app.exec();
}

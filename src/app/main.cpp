#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>

#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/MainWindow.h"
#include "backend/UxPlayReceiver.h"
#include "platform/DependencyDiagnostics.h"
#include "platform/WindowsHotkeyService.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

#if AIRPLAY_WITH_UXPLAY
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
#endif

    WindowsHotkeyService hotkeys;
    const QString settingsPath = QCoreApplication::applicationDirPath() + "/airplay-settings.json";
    const AppSettings settings = AppSettingsStore(settingsPath).loadOrDefaults();

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

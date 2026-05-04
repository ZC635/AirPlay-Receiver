#include <QApplication>
#include <QCoreApplication>

#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/MainWindow.h"
#include "backend/UxPlayReceiver.h"
#include "platform/WindowsHotkeyService.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    WindowsHotkeyService hotkeys;
    const QString settingsPath = QCoreApplication::applicationDirPath() + "/airplay-settings.json";
    const AppSettings settings = AppSettingsStore(settingsPath).loadOrDefaults();

#if AIRPLAY_WITH_UXPLAY
    UxPlayReceiverConfig config;
    config.videoSink = "d3d11videosink";
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

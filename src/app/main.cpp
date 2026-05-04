#include <QApplication>

#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "backend/UxPlayReceiver.h"
#include "platform/WindowsHotkeyService.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    WindowsHotkeyService hotkeys;

#if AIRPLAY_WITH_UXPLAY
    UxPlayReceiverConfig config;
    config.videoSink = "d3d11videosink";
    config.audioSink = "wasapisink";
    UxPlayReceiver receiver(config);
    QObject::connect(&app, &QApplication::aboutToQuit, &receiver, [&receiver] {
        receiver.stop();
    });
    MainWindow window(AppSettings::defaults(), &hotkeys, &receiver);
    receiver.start();
#else
    MainWindow window(AppSettings::defaults(), &hotkeys);
#endif

    window.show();
    return app.exec();
}

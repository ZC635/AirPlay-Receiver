#include <QApplication>

#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "platform/WindowsHotkeyService.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    WindowsHotkeyService hotkeys;
    MainWindow window(AppSettings::defaults(), &hotkeys);
    window.show();
    return app.exec();
}

#include <QtTest/QtTest>
#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "app/ShortcutAction.h"
#include "platform/FakeHotkeyService.h"

class MainWindowSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void constructsWithExpectedTitle() {
        MainWindow window;
        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver"));
    }

    void togglesToolbarVisibility() {
        MainWindow window;
        QVERIFY(window.isToolbarVisible());
        window.toggleToolbarVisibility();
        QVERIFY(!window.isToolbarVisible());
    }

    void togglesAlwaysOnTopState() {
        MainWindow window;
        QVERIFY(!window.isAlwaysOnTopEnabled());
        window.setAlwaysOnTopEnabled(true);
        QVERIFY(window.isAlwaysOnTopEnabled());
    }

    void shortcutTogglesToolbar() {
        auto *hotkeys = new FakeHotkeyService;
        MainWindow window(AppSettings::defaults(), hotkeys);
        QVERIFY(window.isToolbarVisible());
        emit hotkeys->activated(ShortcutAction::ToggleToolbar);
        QVERIFY(!window.isToolbarVisible());
    }
};

QTEST_MAIN(MainWindowSmokeTest)
#include "MainWindowSmokeTest.moc"

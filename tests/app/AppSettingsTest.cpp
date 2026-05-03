#include <QtTest/QtTest>
#include "app/AppSettings.h"

class AppSettingsTest : public QObject {
    Q_OBJECT

private slots:
    void defaultShortcutsContainRequestedActions() {
        const AppSettings settings = AppSettings::defaults();
        QCOMPARE(settings.shortcuts().size(), 4);
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::VolumeUp).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::VolumeDown).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleToolbar).isValid());
    }

    void rejectsDuplicateShortcuts() {
        AppSettings settings = AppSettings::defaults();
        const QKeySequence duplicate("Ctrl+Alt+T");
        settings.setShortcut(ShortcutAction::ToggleAlwaysOnTop, duplicate);
        settings.setShortcut(ShortcutAction::ToggleToolbar, duplicate);
        QVERIFY(!settings.validateShortcuts().isEmpty());
    }
};

QTEST_MAIN(AppSettingsTest)
#include "AppSettingsTest.moc"

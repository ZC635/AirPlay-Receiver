#include <QtTest/QtTest>
#include <QTemporaryDir>
#include "app/AppSettingsStore.h"

class AppSettingsStoreTest : public QObject {
    Q_OBJECT

private slots:
    void savesAndLoadsShortcuts() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H"));

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
    }
};

QTEST_MAIN(AppSettingsStoreTest)
#include "AppSettingsStoreTest.moc"

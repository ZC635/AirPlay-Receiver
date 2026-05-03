#include <QtTest/QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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

    void savesShortcutsAsPortableText() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H"));

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonObject shortcuts = root.value("shortcuts").toObject();

        QCOMPARE(shortcuts.value("toggleToolbar").toString(), QKeySequence("Ctrl+Shift+H").toString(QKeySequence::PortableText));
    }
};

QTEST_MAIN(AppSettingsStoreTest)
#include "AppSettingsStoreTest.moc"

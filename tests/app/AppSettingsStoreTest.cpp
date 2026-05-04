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

    void savesAndLoadsVolume() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setVolume(35);

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.volume(), 35);
    }

    void malformedVolumeFallsBackToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"volume":"loud"})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.volume(), AppSettings::defaults().volume());
    }

    void savesAndLoadsReceiverName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setReceiverName("Desk Receiver");

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.receiverName(), QString("Desk Receiver"));
    }

    void malformedReceiverNameFallsBackToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"receiverName":"   "})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.receiverName(), AppSettings::defaults().receiverName());
    }

    void nonStringReceiverNameFallsBackToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"receiverName":123})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.receiverName(), AppSettings::defaults().receiverName());
    }

    void saveReturnsFalseForDirectoryPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        AppSettingsStore store(dir.path());

        QVERIFY(!store.save(AppSettings::defaults()));
    }
};

QTEST_MAIN(AppSettingsStoreTest)
#include "AppSettingsStoreTest.moc"

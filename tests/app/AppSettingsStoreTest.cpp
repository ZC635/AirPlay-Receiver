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

    void savesAndLoadsAspectRatioLock() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setAspectRatioLock(true);

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        const AppSettings loaded = store.loadOrDefaults();
        QVERIFY(loaded.aspectRatioLock());
    }

    void malformedAspectRatioLockFallsBackToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"aspectRatioLock":"yes"})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.aspectRatioLock(), AppSettings::defaults().aspectRatioLock());
    }

    void savesAndLoadsVideoFitMode() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setVideoFitMode(true);

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        const AppSettings loaded = store.loadOrDefaults();
        QVERIFY(loaded.videoFitMode());
    }

    void malformedVideoFitModeFallsBackToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"videoFitMode":"yes"})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.videoFitMode(), AppSettings::defaults().videoFitMode());
    }

    void corruptJsonReturnsDefaults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{not valid json at all!!!") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        const AppSettings defaults = AppSettings::defaults();
        QCOMPARE(loaded.volume(), defaults.volume());
        QCOMPARE(loaded.receiverName(), defaults.receiverName());
        QCOMPARE(loaded.aspectRatioLock(), defaults.aspectRatioLock());
        QCOMPARE(loaded.videoFitMode(), defaults.videoFitMode());
        QCOMPARE(loaded.shortcuts().size(), defaults.shortcuts().size());
        for (const ShortcutBinding &binding : defaults.shortcuts()) {
            QCOMPARE(loaded.shortcutFor(binding.action), binding.sequence);
        }
    }

    void validJsonArrayReturnsDefaults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("[]") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        const AppSettings defaults = AppSettings::defaults();
        QCOMPARE(loaded.volume(), defaults.volume());
        QCOMPARE(loaded.receiverName(), defaults.receiverName());
        QCOMPARE(loaded.aspectRatioLock(), defaults.aspectRatioLock());
        QCOMPARE(loaded.videoFitMode(), defaults.videoFitMode());
    }

    void invalidShortcutStringKeepsDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"shortcuts":{"toggleToolbar":"garbage_not_a_shortcut"}})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        const AppSettings defaults = AppSettings::defaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), defaults.shortcutFor(ShortcutAction::ToggleToolbar));
    }

    void emptyShortcutStringKeepsDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"shortcuts":{"toggleToolbar":""}})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        const AppSettings defaults = AppSettings::defaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), defaults.shortcutFor(ShortcutAction::ToggleToolbar));
    }

    void multiKeyShortcutStringKeepsDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"shortcuts":{"toggleToolbar":"Ctrl+A, Ctrl+B"}})") > 0);
        file.close();

        AppSettingsStore store(path);
        const AppSettings loaded = store.loadOrDefaults();
        const AppSettings defaults = AppSettings::defaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), defaults.shortcutFor(ShortcutAction::ToggleToolbar));
    }

    void savesAndLoadsVideoQuality() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps15;
        settings.setVideoQuality(quality);

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.videoQuality(), quality);
    }

    void malformedVideoQualityFallsBackToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"videoQuality":{"resolution":"999p","frameRate":"fast"}})") > 0);
        file.close();

        AppSettingsStore store(path);
        QCOMPARE(store.loadOrDefaults().videoQuality(), AppSettings::defaults().videoQuality());
    }

    void savesVideoQualityJsonFormat() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P1080;
        quality.frameRate = VideoFrameRate::Fps30;
        settings.setVideoQuality(quality);

        AppSettingsStore store(path);
        QVERIFY(store.save(settings));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonObject vq = root.value("videoQuality").toObject();

        QCOMPARE(vq.value("resolution").toString(), QString("1080p"));
        QCOMPARE(vq.value("frameRate").toInt(), 30);
        QVERIFY(!vq.contains("codec"));
        QVERIFY(!vq.contains("bitrate"));
    }

    void staleCodecAndBitrateKeysAreIgnored() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"videoQuality":{"resolution":"720p","frameRate":15,"codec":"hevc","bitrate":"extreme"}})") > 0);
        file.close();

        AppSettingsStore store(path);
        const VideoQualitySettings loaded = store.loadOrDefaults().videoQuality();
        QCOMPARE(loaded.resolution, VideoResolution::P720);
        QCOMPARE(loaded.frameRate, VideoFrameRate::Fps15);
    }

    void malformedVideoQualityResolutionDefaults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"videoQuality":{"resolution":"banana","frameRate":30}})") > 0);
        file.close();

        AppSettingsStore store(path);
        QCOMPARE(store.loadOrDefaults().videoQuality().resolution, VideoResolution::P1080);
    }

    void malformedVideoQualityFrameRateDefaults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"videoQuality":{"resolution":"540p","frameRate":999}})") > 0);
        file.close();

        AppSettingsStore store(path);
        QCOMPARE(store.loadOrDefaults().videoQuality().frameRate, VideoFrameRate::Fps30);
    }
};

QTEST_MAIN(AppSettingsStoreTest)
#include "AppSettingsStoreTest.moc"

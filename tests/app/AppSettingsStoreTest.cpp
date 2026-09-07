#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include "app/AppSettingsStore.h"

struct FakeSettingsSaveDeviceState {
    bool openResult = true;
    std::function<qint64(const QByteArray &)> bytesWritten = [](const QByteArray &data) {
        return data.size();
    };
    bool commitResult = true;
    QFileDevice::FileError fileError = QFileDevice::NoError;
    QString errorText;
    bool openCalled = false;
    bool writeCalled = false;
    bool commitCalled = false;
    QByteArray writtenData;
};

class FakeSettingsSaveDevice final : public SettingsSaveDevice {
public:
    explicit FakeSettingsSaveDevice(std::shared_ptr<FakeSettingsSaveDeviceState> state)
        : state_(std::move(state)) {}

    bool open() override {
        state_->openCalled = true;
        return state_->openResult;
    }

    qint64 write(const QByteArray &data) override {
        state_->writeCalled = true;
        state_->writtenData = data;
        return state_->bytesWritten(data);
    }

    bool commit() override {
        state_->commitCalled = true;
        return state_->commitResult;
    }

    QFileDevice::FileError error() const override {
        return state_->fileError;
    }

    QString errorString() const override {
        return state_->errorText;
    }

private:
    std::shared_ptr<FakeSettingsSaveDeviceState> state_;
};

class AppSettingsStoreTest : public QObject {
    Q_OBJECT

private slots:
    void saveReportsOpenFailureDetails() {
        const QString path = "settings.json";
        QString factoryPath;
        auto device = std::make_shared<FakeSettingsSaveDeviceState>();
        device->openResult = false;
        device->fileError = QFileDevice::OpenError;
        device->errorText = "cannot open settings";
        AppSettingsStore store(path, [&factoryPath, device](const QString &targetPath) {
            factoryPath = targetPath;
            return std::make_unique<FakeSettingsSaveDevice>(device);
        });

        const AppSettingsSaveResult result = store.save(AppSettings::defaults());

        QVERIFY(!result.success);
        QCOMPARE(result.targetPath, QFileInfo(path).absoluteFilePath());
        QCOMPARE(factoryPath, QFileInfo(path).absoluteFilePath());
        QCOMPARE(result.failureStage, std::optional<AppSettingsSaveStage>(AppSettingsSaveStage::Open));
        QCOMPARE(result.fileError, QFileDevice::OpenError);
        QCOMPARE(result.errorString, QString("cannot open settings"));
        QVERIFY(device->openCalled);
        QVERIFY(!device->writeCalled);
        QVERIFY(!device->commitCalled);
    }

    void saveReportsUnavailableDeviceFactory() {
        const QString path = "settings.json";
        AppSettingsStore store(path, [](const QString &) {
            return std::unique_ptr<SettingsSaveDevice>();
        });

        const AppSettingsSaveResult result = store.save(AppSettings::defaults());

        QVERIFY(!result.success);
        QCOMPARE(result.targetPath, QFileInfo(path).absoluteFilePath());
        QCOMPARE(result.failureStage, std::optional<AppSettingsSaveStage>(AppSettingsSaveStage::Open));
        QCOMPARE(result.fileError, QFileDevice::NoError);
        QCOMPARE(result.errorString, QString("Settings save device factory returned null."));
    }

    void saveReportsWriteFailureDetails() {
        const QString path = "settings.json";
        auto device = std::make_shared<FakeSettingsSaveDeviceState>();
        device->bytesWritten = [](const QByteArray &data) {
            return data.size() - 1;
        };
        device->fileError = QFileDevice::WriteError;
        device->errorText = "cannot write settings";
        AppSettingsStore store(path, [device](const QString &) {
            return std::make_unique<FakeSettingsSaveDevice>(device);
        });

        const AppSettingsSaveResult result = store.save(AppSettings::defaults());

        QVERIFY(!result.success);
        QCOMPARE(result.targetPath, QFileInfo(path).absoluteFilePath());
        QCOMPARE(result.failureStage, std::optional<AppSettingsSaveStage>(AppSettingsSaveStage::Write));
        QCOMPARE(result.fileError, QFileDevice::WriteError);
        QCOMPARE(result.errorString, QString("cannot write settings"));
        QVERIFY(device->openCalled);
        QVERIFY(device->writeCalled);
        QVERIFY(!device->commitCalled);
    }

    void saveReportsCommitFailureDetails() {
        const QString path = "settings.json";
        auto device = std::make_shared<FakeSettingsSaveDeviceState>();
        device->commitResult = false;
        device->fileError = QFileDevice::RenameError;
        device->errorText = "cannot replace settings";
        AppSettingsStore store(path, [device](const QString &) {
            return std::make_unique<FakeSettingsSaveDevice>(device);
        });

        const AppSettingsSaveResult result = store.save(AppSettings::defaults());

        QVERIFY(!result.success);
        QCOMPARE(result.targetPath, QFileInfo(path).absoluteFilePath());
        QCOMPARE(result.failureStage, std::optional<AppSettingsSaveStage>(AppSettingsSaveStage::Commit));
        QCOMPARE(result.fileError, QFileDevice::RenameError);
        QCOMPARE(result.errorString, QString("cannot replace settings"));
        QVERIFY(device->openCalled);
        QVERIFY(device->writeCalled);
        QVERIFY(device->commitCalled);
    }

    void saveReportsSuccessDetailsAndWritesJsonToDevice() {
        const QString path = "settings.json";
        QString factoryPath;
        auto device = std::make_shared<FakeSettingsSaveDeviceState>();
        AppSettings settings = AppSettings::defaults();
        settings.setReceiverName("Desk Receiver");
        AppSettingsStore store(path, [&factoryPath, device](const QString &targetPath) {
            factoryPath = targetPath;
            return std::make_unique<FakeSettingsSaveDevice>(device);
        });

        const AppSettingsSaveResult result = store.save(settings);

        QVERIFY(result.success);
        QCOMPARE(result.targetPath, QFileInfo(path).absoluteFilePath());
        QCOMPARE(factoryPath, QFileInfo(path).absoluteFilePath());
        QVERIFY(!result.failureStage.has_value());
        QCOMPARE(result.fileError, QFileDevice::NoError);
        QVERIFY(result.errorString.isEmpty());
        QVERIFY(device->openCalled);
        QVERIFY(device->writeCalled);
        QVERIFY(device->commitCalled);
        const QJsonDocument document = QJsonDocument::fromJson(device->writtenData);
        QVERIFY(document.isObject());
        QCOMPARE(document.object().value("receiverName").toString(), QString("Desk Receiver"));
    }

    void savesAndLoadsShortcuts() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H"));

        AppSettingsStore store(path);
        QVERIFY(store.save(settings).success);

        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
    }

    void savesAndLoadsRecordingSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        const QString outputDirectory = dir.filePath("captures/../recordings");
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingFormat(RecordingFormat::Mp4);
        settings.setRecordingOutputDirectory(outputDirectory);
        settings.setShowRecordingCompletionMessage(false);
        settings.setShortcut(ShortcutAction::ToggleRecording, QKeySequence("Ctrl+Shift+R"));

        AppSettingsStore store(path);
        QVERIFY(store.save(settings).success);

        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.recordingFormat(), RecordingFormat::Mp4);
        QCOMPARE(loaded.recordingOutputDirectory(), QDir::cleanPath(QFileInfo(outputDirectory).absoluteFilePath()));
        QVERIFY(!loaded.showRecordingCompletionMessage());
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleRecording), QKeySequence("Ctrl+Shift+R"));
    }

    void savesRecordingJsonSchemaAndPortableShortcut() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingOutputDirectory(dir.filePath("recordings"));
        settings.setShowRecordingCompletionMessage(false);
        settings.setShortcut(ShortcutAction::ToggleRecording, QKeySequence("Ctrl+Shift+R"));

        AppSettingsStore store(path);
        QVERIFY(store.save(settings).success);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonObject recording = root.value("recording").toObject();
        QCOMPARE(recording.keys(), QStringList({"format", "outputDirectory", "showCompletionMessage"}));
        QCOMPARE(recording.value("format").toString(), QString("mp4"));
        QCOMPARE(recording.value("outputDirectory").toString(), settings.recordingOutputDirectory());
        QVERIFY(!recording.value("showCompletionMessage").toBool());
        QCOMPARE(root.value("shortcuts").toObject().value("toggleRecording").toString(),
                 QKeySequence("Ctrl+Shift+R").toString(QKeySequence::PortableText));
    }

    void oldJsonWithoutRecordingKeysUsesDefaults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"receiverName":"Legacy Receiver","shortcuts":{}})") > 0);
        file.close();

        const AppSettings loaded = AppSettingsStore(path).loadOrDefaults();
        const AppSettings defaults = AppSettings::defaults();
        QCOMPARE(loaded.recordingFormat(), defaults.recordingFormat());
        QCOMPARE(loaded.recordingOutputDirectory(), defaults.recordingOutputDirectory());
        QCOMPARE(loaded.showRecordingCompletionMessage(), defaults.showRecordingCompletionMessage());
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleRecording),
                 defaults.shortcutFor(ShortcutAction::ToggleRecording));
    }

    void unknownRecordingFormatFallsBackToMp4() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"recording":{"format":"webm"}})") > 0);
        file.close();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().recordingFormat(), RecordingFormat::Mp4);
    }

    void nonStringRecordingFormatFallsBackToMp4() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"recording":{"format":42}})") > 0);
        file.close();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().recordingFormat(), RecordingFormat::Mp4);
    }

    void recordingOutputDirectoryLoadsAsAbsoluteCleanPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"recording":{"outputDirectory":"captures/../recordings"}})") > 0);
        file.close();

        const QString expected = QDir::cleanPath(QFileInfo("captures/../recordings").absoluteFilePath());
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().recordingOutputDirectory(), expected);
    }

    void nonBooleanCompletionMessageFallsBackToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"recording":{"showCompletionMessage":"no"}})") > 0);
        file.close();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().showRecordingCompletionMessage(),
                 AppSettings::defaults().showRecordingCompletionMessage());
    }

    void invalidToggleRecordingShortcutKeepsDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"shortcuts":{"toggleRecording":"not_a_hotkey"}})") > 0);
        file.close();

        const AppSettings loaded = AppSettingsStore(path).loadOrDefaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleRecording),
                 AppSettings::defaults().shortcutFor(ShortcutAction::ToggleRecording));
    }

    void savesShortcutsAsPortableText() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H"));

        AppSettingsStore store(path);
        QVERIFY(store.save(settings).success);

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
        QVERIFY(store.save(settings).success);

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
        QVERIFY(store.save(settings).success);

        const AppSettings loaded = store.loadOrDefaults();
        QCOMPARE(loaded.receiverName(), QString("Desk Receiver"));
    }

    void missingLanguageFieldUsesSystem() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(R"({"receiverName":"Legacy Receiver"})") > 0);
        file.close();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().language(), QString("system"));
    }

    void languageRoundTrips() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setLanguage("zh-CN");

        AppSettingsStore store(path);
        QVERIFY(store.save(settings).success);

        QCOMPARE(store.loadOrDefaults().language(), QString("zh-CN"));
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

        QVERIFY(!store.save(AppSettings::defaults()).success);
    }

    void savesAndLoadsAspectRatioLock() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setAspectRatioLock(true);

        AppSettingsStore store(path);
        QVERIFY(store.save(settings).success);

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
        QVERIFY(store.save(settings).success);

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
        QVERIFY(store.save(settings).success);

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
        QVERIFY(store.save(settings).success);

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

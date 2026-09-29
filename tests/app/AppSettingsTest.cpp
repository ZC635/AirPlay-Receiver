#include <QtTest/QtTest>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include "app/AppSettings.h"
#include "backend/RecordingTypes.h"
#include "backend/VideoQualitySettings.h"

class AppSettingsTest : public QObject {
    Q_OBJECT

private slots:
    void videoQualityDefaultsAreP1080Fps30() {
        const AppSettings settings = AppSettings::defaults();
        const VideoQualitySettings quality = settings.videoQuality();
        QCOMPARE(quality.resolution, VideoResolution::P1080);
        QCOMPARE(quality.frameRate, VideoFrameRate::Fps30);
    }

    void videoQualitySetterAndGetter() {
        AppSettings settings = AppSettings::defaults();
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps15;

        settings.setVideoQuality(quality);

        QCOMPARE(settings.videoQuality().resolution, VideoResolution::P720);
        QCOMPARE(settings.videoQuality().frameRate, VideoFrameRate::Fps15);
    }

    void defaultShortcutsContainRequestedActions() {
        const AppSettings settings = AppSettings::defaults();
        QCOMPARE(settings.shortcuts().size(), 7);
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::VolumeUp).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::VolumeDown).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleToolbar).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleAspectRatio).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleVideoFit).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleRecording).isValid());
    }

    void defaultToggleVideoFitShortcutIsCorrect() {
        const AppSettings settings = AppSettings::defaults();
        QCOMPARE(settings.shortcutFor(ShortcutAction::ToggleVideoFit), QKeySequence("Ctrl+Alt+F"));
    }

    void recordingDefaultsAreMp4MoviesFolderAndCompletionMessage() {
        const AppSettings settings = AppSettings::defaults();
        const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        const QString expectedBase = movies.isEmpty()
            ? QDir(QDir::homePath()).filePath("Videos")
            : movies;

        QCOMPARE(settings.recordingFormat(), RecordingFormat::Mp4);
        QCOMPARE(settings.recordingOutputDirectory(),
                 QDir::cleanPath(QDir(expectedBase).filePath("AirPlay Receiver Recording")));
        QVERIFY(settings.showRecordingCompletionMessage());
    }

    void recordingSettersStoreValuesAndNormalizeOutputDirectory() {
        AppSettings settings = AppSettings::defaults();
        const QString path = QDir::current().filePath("recordings/../saved recordings");

        settings.setRecordingFormat(RecordingFormat::Mp4);
        settings.setRecordingOutputDirectory(path);
        settings.setShowRecordingCompletionMessage(false);

        QCOMPARE(settings.recordingFormat(), RecordingFormat::Mp4);
        QCOMPARE(settings.recordingOutputDirectory(),
                 QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        QVERIFY(!settings.showRecordingCompletionMessage());
    }

    void defaultToggleRecordingShortcutIsCtrlAltR() {
        const AppSettings settings = AppSettings::defaults();

        QCOMPARE(settings.shortcuts().size(), 7);
        QCOMPARE(settings.shortcutFor(ShortcutAction::ToggleRecording), QKeySequence("Ctrl+Alt+R"));
    }

    void rejectsDuplicateShortcuts() {
        AppSettings settings = AppSettings::defaults();
        const QKeySequence duplicate("Ctrl+Alt+T");
        settings.setShortcut(ShortcutAction::ToggleAlwaysOnTop, duplicate);
        settings.setShortcut(ShortcutAction::ToggleToolbar, duplicate);
        QVERIFY(!settings.validateShortcuts().isEmpty());
    }

    void defaultVolumeIsMax() {
        const AppSettings settings = AppSettings::defaults();
        QCOMPARE(settings.volume(), 100);
    }

    void clampsVolumeRange() {
        AppSettings settings = AppSettings::defaults();
        settings.setVolume(-5);
        QCOMPARE(settings.volume(), 0);

        settings.setVolume(125);
        QCOMPARE(settings.volume(), 100);
    }

    void defaultReceiverNameIsAirPlayReceiver() {
        const AppSettings settings = AppSettings::defaults();
        QCOMPARE(settings.receiverName(), QString("AirPlay Receiver"));
    }

    void defaultsToSystemLanguage() {
        QCOMPARE(AppSettings::defaults().language(), QString("system"));
    }

    void emptyLanguageNormalizesToSystem() {
        AppSettings settings = AppSettings::defaults();
        settings.setLanguage("   ");

        QCOMPARE(settings.language(), QString("system"));
    }

    void languageTagIsTrimmedButNotRestricted() {
        AppSettings settings = AppSettings::defaults();
        settings.setLanguage("  fr-CA  ");

        QCOMPARE(settings.language(), QString("fr-CA"));
    }

    void storesTrimmedReceiverName() {
        AppSettings settings = AppSettings::defaults();
        settings.setReceiverName("  Living Room PC  ");
        QCOMPARE(settings.receiverName(), QString("Living Room PC"));
    }

    void rejectsEmptyReceiverName() {
        AppSettings settings = AppSettings::defaults();
        settings.setReceiverName("   ");
        QVERIFY(settings.validateGeneral().join('\n').contains("Receiver name"));
    }

    void aspectRatioLockDefaultsToTrue() {
        AppSettings settings = AppSettings::defaults();
        QVERIFY(settings.aspectRatioLock());
    }

    void aspectRatioLockSetterAndGetter() {
        AppSettings settings = AppSettings::defaults();
        settings.setAspectRatioLock(true);
        QVERIFY(settings.aspectRatioLock());
        settings.setAspectRatioLock(false);
        QVERIFY(!settings.aspectRatioLock());
    }

    void videoFitModeDefaultsToTrue() {
        AppSettings settings = AppSettings::defaults();
        QVERIFY(settings.videoFitMode());
    }

    void videoFitModeSetterAndGetter() {
        AppSettings settings = AppSettings::defaults();
        settings.setVideoFitMode(true);
        QVERIFY(settings.videoFitMode());
        settings.setVideoFitMode(false);
        QVERIFY(!settings.videoFitMode());
    }

    void toolbarHoverRevealDefaultsOnAndCanBeDisabled() {
        AppSettings settings = AppSettings::defaults();
        QVERIFY(settings.toolbarHoverReveal());
        settings.setToolbarHoverReveal(false);
        QVERIFY(!settings.toolbarHoverReveal());
    }
};

QTEST_MAIN(AppSettingsTest)
#include "AppSettingsTest.moc"

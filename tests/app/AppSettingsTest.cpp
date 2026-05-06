#include <QtTest/QtTest>
#include "app/AppSettings.h"

class AppSettingsTest : public QObject {
    Q_OBJECT

private slots:
    void defaultShortcutsContainRequestedActions() {
        const AppSettings settings = AppSettings::defaults();
        QCOMPARE(settings.shortcuts().size(), 6);
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::VolumeUp).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::VolumeDown).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleToolbar).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleAspectRatio).isValid());
        QVERIFY(settings.shortcutFor(ShortcutAction::ToggleVideoFit).isValid());
    }

    void defaultToggleVideoFitShortcutIsCorrect() {
        const AppSettings settings = AppSettings::defaults();
        QCOMPARE(settings.shortcutFor(ShortcutAction::ToggleVideoFit), QKeySequence("Ctrl+Alt+F"));
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

    void aspectRatioLockDefaultsToFalse() {
        AppSettings settings = AppSettings::defaults();
        QVERIFY(!settings.aspectRatioLock());
    }

    void aspectRatioLockSetterAndGetter() {
        AppSettings settings = AppSettings::defaults();
        settings.setAspectRatioLock(true);
        QVERIFY(settings.aspectRatioLock());
        settings.setAspectRatioLock(false);
        QVERIFY(!settings.aspectRatioLock());
    }

    void videoFitModeDefaultsToFalse() {
        AppSettings settings = AppSettings::defaults();
        QVERIFY(!settings.videoFitMode());
    }

    void videoFitModeSetterAndGetter() {
        AppSettings settings = AppSettings::defaults();
        settings.setVideoFitMode(true);
        QVERIFY(settings.videoFitMode());
        settings.setVideoFitMode(false);
        QVERIFY(!settings.videoFitMode());
    }
};

QTEST_MAIN(AppSettingsTest)
#include "AppSettingsTest.moc"

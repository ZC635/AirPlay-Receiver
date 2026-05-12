#include <QtTest/QtTest>
#include "app/ShortcutActionKey.h"

class ShortcutActionKeyTest : public QObject {
    Q_OBJECT

private slots:
    void mapsEveryActionToExpectedString() {
        QCOMPARE(shortcutActionKey(ShortcutAction::ToggleAlwaysOnTop), QString("toggleAlwaysOnTop"));
        QCOMPARE(shortcutActionKey(ShortcutAction::VolumeUp), QString("volumeUp"));
        QCOMPARE(shortcutActionKey(ShortcutAction::VolumeDown), QString("volumeDown"));
        QCOMPARE(shortcutActionKey(ShortcutAction::ToggleToolbar), QString("toggleToolbar"));
        QCOMPARE(shortcutActionKey(ShortcutAction::ToggleAspectRatio), QString("toggleAspectRatio"));
        QCOMPARE(shortcutActionKey(ShortcutAction::ToggleVideoFit), QString("toggleVideoFit"));
    }
};

QTEST_MAIN(ShortcutActionKeyTest)
#include "ShortcutActionKeyTest.moc"

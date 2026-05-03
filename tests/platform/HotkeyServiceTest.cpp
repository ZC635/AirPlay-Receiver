#include <QtTest/QtTest>
#include "platform/WindowsHotkeyService.h"

class HotkeyServiceTest : public QObject {
    Q_OBJECT

private slots:
    void convertsCtrlAltT() {
        const auto native = WindowsHotkeyService::toNativeHotkey(QKeySequence("Ctrl+Alt+T"));
        QVERIFY(native.has_value());
        QVERIFY(native->modifiers != 0);
        QVERIFY(native->virtualKey != 0);
    }

    void rejectsEmptyShortcut() {
        QVERIFY(!WindowsHotkeyService::toNativeHotkey(QKeySequence()).has_value());
    }
};

QTEST_MAIN(HotkeyServiceTest)
#include "HotkeyServiceTest.moc"

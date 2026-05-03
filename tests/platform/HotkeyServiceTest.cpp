#include <QtTest/QtTest>

#include "app/AppSettings.h"
#include "platform/WindowsHotkeyService.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

    void convertsDefaultShortcuts() {
        const AppSettings settings = AppSettings::defaults();
        for (const ShortcutBinding &binding : settings.shortcuts()) {
            const auto native = WindowsHotkeyService::toNativeHotkey(binding.sequence);
            QVERIFY2(native.has_value(), qPrintable(binding.sequence.toString(QKeySequence::PortableText)));
            QVERIFY(native->modifiers != 0);
            QVERIFY(native->virtualKey != 0);
        }
    }

    void convertsArrowShortcutsToExpectedVirtualKeys() {
        const auto up = WindowsHotkeyService::toNativeHotkey(QKeySequence("Ctrl+Alt+Up"));
        const auto down = WindowsHotkeyService::toNativeHotkey(QKeySequence("Ctrl+Alt+Down"));

        QVERIFY(up.has_value());
        QVERIFY(down.has_value());
#ifdef Q_OS_WIN
        QCOMPARE(up->virtualKey, static_cast<unsigned int>(VK_UP));
        QCOMPARE(down->virtualKey, static_cast<unsigned int>(VK_DOWN));
#else
        QVERIFY(up->virtualKey != 0);
        QVERIFY(down->virtualKey != 0);
#endif
    }
};

QTEST_MAIN(HotkeyServiceTest)
#include "HotkeyServiceTest.moc"

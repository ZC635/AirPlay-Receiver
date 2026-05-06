#include <QtTest/QtTest>

#include "app/AppSettings.h"
#include "app/ShortcutBinding.h"
#include "platform/WindowsHotkeyService.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

    void rejectsMultiKeySequence() {
        QVERIFY(!WindowsHotkeyService::toNativeHotkey(QKeySequence("Ctrl+K, Ctrl+C")).has_value());
        QVERIFY(!WindowsHotkeyService::toNativeHotkey(QKeySequence("Ctrl+A, Ctrl+B, Ctrl+C")).has_value());
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
        QCOMPARE(up->virtualKey, static_cast<unsigned int>(VK_UP));
        QCOMPARE(down->virtualKey, static_cast<unsigned int>(VK_DOWN));
    }

    void registerShortcutRejectsInvalidSequence() {
        WindowsHotkeyService service;
        QVERIFY(!service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence()));
        QVERIFY(!service.registerShortcut(ShortcutAction::VolumeUp, QKeySequence("F99")));
    }

    void registerShortcutPreservesExistingOnInvalidReregistration() {
        WindowsHotkeyService service;
        service.registrations_.insert(1, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});
        QVERIFY(!service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence()));
        QCOMPARE(service.registrations_.size(), 1);
        QVERIFY(service.registrations_.contains(1));
        QCOMPARE(service.registrations_.value(1).action, ShortcutAction::ToggleToolbar);
        QCOMPARE(service.registrations_.value(1).sequence, QKeySequence("Ctrl+Shift+Z"));
    }

    void nativeEventFilterSetsResultOnHandledHotkey() {
        WindowsHotkeyService service;
        const int id = 1;
        service.registrations_.insert(id, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});

        MSG msg{};
        msg.message = WM_HOTKEY;
        msg.wParam = id;
        qintptr result = 0;

        const bool handled = service.nativeEventFilter(QByteArray(), &msg, &result);

        QVERIFY(handled);
        QCOMPARE(result, static_cast<qintptr>(TRUE));
    }

    void nativeEventFilterIgnoresUnregisteredHotkeyId() {
        WindowsHotkeyService service;
        service.registrations_.insert(1, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});

        MSG msg{};
        msg.message = WM_HOTKEY;
        msg.wParam = 999;
        qintptr result = 0;

        const bool handled = service.nativeEventFilter(QByteArray(), &msg, &result);

        QVERIFY(!handled);
        QCOMPARE(result, static_cast<qintptr>(0));
    }

    void nativeEventFilterPassesThroughNonHotkeyMessages() {
        WindowsHotkeyService service;
        MSG msg{};
        msg.message = WM_PAINT;
        qintptr result = 0;

        const bool handled = service.nativeEventFilter(QByteArray(), &msg, &result);

        QVERIFY(!handled);
        QCOMPARE(result, static_cast<qintptr>(0));
    }
};

QTEST_MAIN(HotkeyServiceTest)
#include "HotkeyServiceTest.moc"

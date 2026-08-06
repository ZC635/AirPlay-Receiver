#include <QtTest/QtTest>

#include "app/AppSettings.h"
#include "app/ShortcutBinding.h"
#include "platform/WindowsHotkeyService.h"

#include <QStringList>

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

    void defaultRecordingShortcutIsCtrlAltR() {
        QCOMPARE(AppSettings::defaults().shortcutFor(ShortcutAction::ToggleRecording),
                 QKeySequence("Ctrl+Alt+R"));
    }

    void recordingShortcutParticipatesInConflictValidation() {
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleRecording,
                             settings.shortcutFor(ShortcutAction::ToggleToolbar));

        const QStringList errors = settings.validateShortcuts();

        QVERIFY(errors.contains(QStringLiteral("Duplicate shortcut: Ctrl+Alt+B")));
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
        const auto empty = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence());
        QVERIFY(!empty.registered);
        QVERIFY(empty.error.has_value());
        QVERIFY(!empty.error->nativeCode.has_value());

        const auto unsupported = service.registerShortcut(ShortcutAction::VolumeUp, QKeySequence("F99"));
        QVERIFY(!unsupported.registered);
        QVERIFY(unsupported.error.has_value());
        QVERIFY(!unsupported.error->nativeCode.has_value());
    }

    void registerShortcutPreservesExistingOnInvalidReregistration() {
        WindowsHotkeyService service;
        const int id = static_cast<int>(ShortcutAction::ToggleToolbar) + 1;
        service.registrations_.insert(id, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});
        const auto result = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence());
        QVERIFY(!result.registered);
        QVERIFY(result.error.has_value());
        QCOMPARE(service.registrations_.size(), 1);
        QVERIFY(service.registrations_.contains(id));
        QCOMPARE(service.registrations_.value(id).action, ShortcutAction::ToggleToolbar);
        QCOMPARE(service.registrations_.value(id).sequence, QKeySequence("Ctrl+Shift+Z"));
    }

    void failedReplacementRestoresPreviousBindingAndPreservesCandidateError() {
        QStringList calls;
        quint32 currentError = 0;
        HotkeyNativeOperations operations{
            [&calls, &currentError](int, unsigned int, unsigned int) {
                calls.append("register");
                if (calls.count("register") == 1) {
                    currentError = 1409;
                    return false;
                }
                return true;
            },
            [&calls](int) { calls.append("unregister"); },
            [&calls, &currentError] {
                calls.append(QString("lastError:%1").arg(currentError));
                return currentError;
            },
            [&calls](quint32 error) {
                calls.append(QString("format:%1").arg(error));
                return error == 1409 ? QString("Hot key is already registered.") : QString();
            },
        };
        WindowsHotkeyService service(operations);
        const int id = static_cast<int>(ShortcutAction::ToggleToolbar) + 1;
        service.registrations_.insert(id, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});

        const auto result = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+T"));

        QVERIFY(!result.registered);
        QVERIFY(result.previousRestored);
        QVERIFY(result.error.has_value());
        QCOMPARE(result.error->nativeCode, std::optional<quint32>(1409));
        QCOMPARE(result.error->message, QString("Hot key is already registered."));
        QVERIFY(!result.recoveryError.has_value());
        QCOMPARE(service.registrations_.value(id).sequence, QKeySequence("Ctrl+Shift+Z"));
        QCOMPARE(calls, QStringList({"unregister", "register", "lastError:1409", "format:1409", "register"}));
    }

    void failedReplacementReportsRecoveryErrorWhenPreviousBindingCannotBeRestored() {
        QStringList calls;
        quint32 currentError = 0;
        int registerCalls = 0;
        HotkeyNativeOperations operations{
            [&calls, &currentError, &registerCalls](int, unsigned int, unsigned int) {
                calls.append("register");
                ++registerCalls;
                currentError = registerCalls == 1 ? 1409 : 5;
                return false;
            },
            [&calls](int) { calls.append("unregister"); },
            [&calls, &currentError] {
                calls.append(QString("lastError:%1").arg(currentError));
                return currentError;
            },
            [&calls](quint32 error) {
                calls.append(QString("format:%1").arg(error));
                return error == 1409 ? QString("Hot key is already registered.")
                                     : QString("Access is denied.");
            },
        };
        WindowsHotkeyService service(operations);
        const int id = static_cast<int>(ShortcutAction::ToggleToolbar) + 1;
        service.registrations_.insert(id, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});

        const auto result = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+T"));

        QVERIFY(!result.registered);
        QVERIFY(!result.previousRestored);
        QVERIFY(result.error.has_value());
        QCOMPARE(result.error->nativeCode, std::optional<quint32>(1409));
        QCOMPARE(result.error->message, QString("Hot key is already registered."));
        QVERIFY(result.recoveryError.has_value());
        QCOMPARE(result.recoveryError->nativeCode, std::optional<quint32>(5));
        QCOMPARE(result.recoveryError->message, QString("Access is denied."));
        QVERIFY(!service.registrations_.contains(id));
        QCOMPARE(calls, QStringList({"unregister", "register", "lastError:1409", "format:1409", "register", "lastError:5", "format:5"}));
    }

    void reregisteringSameSequenceIsUnchangedWithoutNativeCalls() {
        int registerCalls = 0;
        int unregisterCalls = 0;
        HotkeyNativeOperations operations{
            [&registerCalls](int, unsigned int, unsigned int) { ++registerCalls; return true; },
            [&unregisterCalls](int) { ++unregisterCalls; },
            [] { return quint32(0); },
            [](quint32) { return QString(); },
        };
        WindowsHotkeyService service(operations);
        const int id = static_cast<int>(ShortcutAction::ToggleToolbar) + 1;
        service.registrations_.insert(id, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});

        const auto result = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z"));

        QVERIFY(result.registered);
        QVERIFY(result.unchanged);
        QCOMPARE(registerCalls, 0);
        QCOMPARE(unregisterCalls, 0);
    }

    void invalidCandidatePreservesExistingBindingWithoutNativeCalls() {
        int registerCalls = 0;
        int unregisterCalls = 0;
        HotkeyNativeOperations operations{
            [&registerCalls](int, unsigned int, unsigned int) { ++registerCalls; return true; },
            [&unregisterCalls](int) { ++unregisterCalls; },
            [] { return quint32(0); },
            [](quint32) { return QString(); },
        };
        WindowsHotkeyService service(operations);
        const int id = static_cast<int>(ShortcutAction::ToggleToolbar) + 1;
        service.registrations_.insert(id, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});

        const auto result = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence());

        QVERIFY(!result.registered);
        QVERIFY(result.error.has_value());
        QCOMPARE(service.registrations_.value(id).sequence, QKeySequence("Ctrl+Shift+Z"));
        QCOMPARE(registerCalls, 0);
        QCOMPARE(unregisterCalls, 0);
    }

    void nativeFailureUsesStableFallbackWhenSystemMessageIsUnavailable() {
        HotkeyNativeOperations operations{
            [](int, unsigned int, unsigned int) { return false; },
            [](int) {},
            [] { return quint32(1234); },
            [](quint32) { return QString(); },
        };
        WindowsHotkeyService service(operations);

        const auto result = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+T"));

        QVERIFY(!result.registered);
        QVERIFY(result.error.has_value());
        QCOMPARE(result.error->nativeCode, std::optional<quint32>(1234));
        QCOMPARE(result.error->message, QString("Windows hotkey registration failed (error 1234)."));
    }

    void successfulReplacementLeavesOtherActionsRegistered() {
        int unregisterCalls = 0;
        HotkeyNativeOperations operations{
            [](int, unsigned int, unsigned int) { return true; },
            [&unregisterCalls](int) { ++unregisterCalls; },
            [] { return quint32(0); },
            [](quint32) { return QString(); },
        };
        WindowsHotkeyService service(operations);
        const int toolbarId = static_cast<int>(ShortcutAction::ToggleToolbar) + 1;
        service.registrations_.insert(toolbarId, {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+Z")});
        service.registrations_.insert(2, {ShortcutAction::VolumeUp, QKeySequence("Ctrl+Shift+U")});

        const auto result = service.registerShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+T"));

        QVERIFY(result.registered);
        QVERIFY(!result.unchanged);
        QCOMPARE(unregisterCalls, 1);
        QCOMPARE(service.registrations_.value(toolbarId).sequence, QKeySequence("Ctrl+Alt+T"));
        QCOMPARE(service.registrations_.value(2).sequence, QKeySequence("Ctrl+Shift+U"));
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

    void nativeEventFilterDispatchesRecordingAction() {
        WindowsHotkeyService service;
        const int id = static_cast<int>(ShortcutAction::ToggleRecording) + 1;
        service.registrations_.insert(
            id, {ShortcutAction::ToggleRecording, QKeySequence("Ctrl+Alt+R")});
        ShortcutAction activatedAction = ShortcutAction::ToggleToolbar;
        int activatedCount = 0;
        connect(&service, &HotkeyService::activated, &service,
                [&](ShortcutAction action) {
                    activatedAction = action;
                    ++activatedCount;
                });

        MSG msg{};
        msg.message = WM_HOTKEY;
        msg.wParam = id;
        qintptr result = 0;

        QVERIFY(service.nativeEventFilter(QByteArray(), &msg, &result));
        QCOMPARE(activatedCount, 1);
        QCOMPARE(activatedAction, ShortcutAction::ToggleRecording);
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

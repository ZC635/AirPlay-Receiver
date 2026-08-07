#include <QtTest/QtTest>

#include "app/AppSettings.h"
#include "app/ShortcutBinding.h"
#include "platform/WindowsHotkeyService.h"

#include <QHash>
#include <QStringList>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

class DefaultBatchHotkeyService final : public HotkeyService {
public:
    using HotkeyService::HotkeyService;

    HotkeyRegistrationResult registerShortcut(ShortcutAction action,
                                              const QKeySequence &sequence) override {
        calls.append({action, sequence});
        return {true};
    }

    void unregisterAll() override {}

    struct Call {
        ShortcutAction action;
        QKeySequence sequence;
    };
    QVector<Call> calls;
};

class TrackingNativeHotkeys {
public:
    HotkeyNativeOperations operations() {
        return {
            [this](int id, unsigned int modifiers, unsigned int virtualKey) {
                registerIds.append(id);
                if (reject && reject(id, modifiers, virtualKey)) {
                    lastError = 1409;
                    return false;
                }
                const QString binding = key(modifiers, virtualKey);
                for (auto entry = active.cbegin(); entry != active.cend(); ++entry) {
                    if (entry.key() != id && entry.value() == binding) {
                        lastError = 1409;
                        return false;
                    }
                }
                active.insert(id, binding);
                return true;
            },
            [this](int id) {
                unregisterIds.append(id);
                active.remove(id);
            },
            [this] { return lastError; },
            [](quint32 error) { return QStringLiteral("native error %1").arg(error); },
        };
    }

    void bind(ShortcutAction action, const QKeySequence &sequence) {
        const auto native = WindowsHotkeyService::toNativeHotkey(sequence);
        Q_ASSERT(native.has_value());
        active.insert(static_cast<int>(action) + 1, key(native->modifiers, native->virtualKey));
    }

    static QString key(unsigned int modifiers, unsigned int virtualKey) {
        return QStringLiteral("%1:%2").arg(modifiers).arg(virtualKey);
    }

    QHash<int, QString> active;
    QVector<int> registerIds;
    QVector<int> unregisterIds;
    std::function<bool(int, unsigned int, unsigned int)> reject;
    quint32 lastError = 0;
};

} // namespace

class HotkeyServiceTest : public QObject {
    Q_OBJECT

    static void seed(WindowsHotkeyService *service, TrackingNativeHotkeys *native,
                     ShortcutAction action, const QKeySequence &sequence) {
        const int id = static_cast<int>(action) + 1;
        service->registrations_.insert(id, {action, sequence});
        native->bind(action, sequence);
    }

private slots:
    void batchDefaultPathPreservesRequestAndResultOrder() {
        DefaultBatchHotkeyService service;
        const QVector<HotkeyRegistrationRequest> requests = {
            {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B")},
            {ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up")},
        };

        const QVector<HotkeyActionRegistrationResult> results = service.registerShortcuts(requests);

        QCOMPARE(results.size(), requests.size());
        QCOMPARE(service.calls.size(), requests.size());
        for (qsizetype index = 0; index < requests.size(); ++index) {
            QCOMPARE(service.calls.at(index).action, requests.at(index).action);
            QCOMPARE(service.calls.at(index).sequence, requests.at(index).sequence);
            QCOMPARE(results.at(index).action, requests.at(index).action);
            QCOMPARE(results.at(index).attemptedSequence, requests.at(index).sequence);
            QVERIFY(results.at(index).registration.registered);
        }
    }

    void swapsTwoActiveShortcutsWithoutUnregisterAll() {
        TrackingNativeHotkeys native;
        WindowsHotkeyService service(native.operations());
        seed(&service, &native, ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B"));
        seed(&service, &native, ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up"));

        const auto results = service.registerShortcuts({
            {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+Up")},
            {ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+B")},
        });

        QCOMPARE(results.size(), 2);
        QVERIFY(results.at(0).registration.registered);
        QVERIFY(results.at(1).registration.registered);
        QCOMPARE(service.registrations_.value(4).sequence, QKeySequence("Ctrl+Alt+Up"));
        QCOMPARE(service.registrations_.value(2).sequence, QKeySequence("Ctrl+Alt+B"));
        QCOMPARE(native.unregisterIds, QVector<int>({4, 2}));
    }

    void ordersAcyclicMoveBeforeItsDependentRequest() {
        TrackingNativeHotkeys native;
        WindowsHotkeyService service(native.operations());
        seed(&service, &native, ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B"));
        seed(&service, &native, ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up"));

        const auto results = service.registerShortcuts({
            {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+Up")},
            {ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+C")},
        });

        QVERIFY(results.at(0).registration.registered);
        QVERIFY(results.at(1).registration.registered);
        QCOMPARE(native.unregisterIds, QVector<int>({2, 4}));
        QCOMPARE(native.registerIds, QVector<int>({2, 4}));
    }

    void failedAcyclicOwnerDoesNotLetDependentClaimItsPreviousShortcut() {
        TrackingNativeHotkeys native;
        WindowsHotkeyService service(native.operations());
        seed(&service, &native, ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B"));
        seed(&service, &native, ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up"));
        native.reject = [](int id, unsigned int, unsigned int) { return id == 2; };

        const auto results = service.registerShortcuts({
            {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+Up")},
            {ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+C")},
        });

        QVERIFY(!results.at(1).registration.registered);
        QVERIFY(results.at(1).registration.recoveryError.has_value());
        QVERIFY(!results.at(0).registration.registered);
        QVERIFY(results.at(0).registration.previousRestored);
        QVERIFY(results.at(0).registration.error.has_value());
        QVERIFY(results.at(0).registration.error->message.contains("could not move"));
        QCOMPARE(service.registrations_.value(4).sequence, QKeySequence("Ctrl+Alt+B"));
        QVERIFY(!service.registrations_.contains(2));
        QCOMPARE(native.unregisterIds, QVector<int>({2}));
        QCOMPARE(native.registerIds, QVector<int>({2, 2}));
    }

    void cycleCandidateFailureRestoresOnlyThatCycle() {
        TrackingNativeHotkeys native;
        WindowsHotkeyService service(native.operations());
        seed(&service, &native, ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B"));
        seed(&service, &native, ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up"));
        seed(&service, &native, ShortcutAction::VolumeDown, QKeySequence("Ctrl+Alt+Down"));
        int volumeUpRegistrations = 0;
        native.reject = [&volumeUpRegistrations](int id, unsigned int, unsigned int) {
            return id == 2 && ++volumeUpRegistrations == 1;
        };

        const auto results = service.registerShortcuts({
            {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+Up")},
            {ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+B")},
            {ShortcutAction::VolumeDown, QKeySequence("Ctrl+Alt+D")},
        });

        QVERIFY(!results.at(0).registration.registered);
        QVERIFY(!results.at(1).registration.registered);
        QVERIFY(results.at(0).registration.previousRestored);
        QVERIFY(results.at(1).registration.previousRestored);
        QVERIFY(results.at(0).registration.error.has_value());
        QVERIFY(results.at(0).registration.error->message.contains("cycle"));
        QVERIFY(results.at(1).registration.error.has_value());
        QVERIFY(results.at(2).registration.registered);
        QCOMPARE(service.registrations_.value(4).sequence, QKeySequence("Ctrl+Alt+B"));
        QCOMPARE(service.registrations_.value(2).sequence, QKeySequence("Ctrl+Alt+Up"));
        QCOMPARE(service.registrations_.value(3).sequence, QKeySequence("Ctrl+Alt+D"));
        QCOMPARE(native.unregisterIds.count(3), 1);
    }

    void cycleRestorationFailureReportsAffectedAction() {
        TrackingNativeHotkeys native;
        WindowsHotkeyService service(native.operations());
        seed(&service, &native, ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B"));
        seed(&service, &native, ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up"));
        int toolbarRegistrations = 0;
        int volumeUpRegistrations = 0;
        native.reject = [&toolbarRegistrations, &volumeUpRegistrations](int id, unsigned int, unsigned int) {
            if (id == 4) {
                ++toolbarRegistrations;
                return toolbarRegistrations == 2;
            }
            return id == 2 && ++volumeUpRegistrations == 1;
        };

        const auto results = service.registerShortcuts({
            {ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+Up")},
            {ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+B")},
        });

        QVERIFY(!results.at(0).registration.registered);
        QVERIFY(results.at(0).registration.recoveryError.has_value());
        QVERIFY(!results.at(1).registration.registered);
        QVERIFY(results.at(1).registration.previousRestored);
        QVERIFY(!service.registrations_.contains(4));
        QCOMPARE(service.registrations_.value(2).sequence, QKeySequence("Ctrl+Alt+Up"));
    }

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

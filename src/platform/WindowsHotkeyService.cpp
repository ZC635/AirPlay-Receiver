#include "platform/WindowsHotkeyService.h"

#include <QCoreApplication>
#include <QString>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
int idForAction(ShortcutAction action) {
    return static_cast<int>(action) + 1;
}

unsigned int toVirtualKey(int key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<unsigned int>('A' + key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<unsigned int>('0' + key - Qt::Key_0);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return static_cast<unsigned int>(0x70 + key - Qt::Key_F1);
    }
    switch (key) {
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Return: return VK_RETURN;
    case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Backspace: return VK_BACK;
    default: return 0;
    }
}

QString unknownHotkeyErrorMessage(quint32 error) {
    return QStringLiteral("Windows hotkey registration failed (error %1).").arg(error);
}

HotkeyNativeOperations defaultNativeOperations() {
    return {
        [](int id, unsigned int modifiers, unsigned int virtualKey) {
            return RegisterHotKey(nullptr, id, modifiers, virtualKey) != FALSE;
        },
        [](int id) { UnregisterHotKey(nullptr, id); },
        [] { return static_cast<quint32>(GetLastError()); },
        [](quint32 error) {
            LPWSTR buffer = nullptr;
            const DWORD length = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
            if (length == 0 || buffer == nullptr) {
                return unknownHotkeyErrorMessage(error);
            }
            const QString message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
            LocalFree(buffer);
            return message.isEmpty() ? unknownHotkeyErrorMessage(error) : message;
        },
    };
}

HotkeyError nativeError(const HotkeyNativeOperations &operations) {
    const quint32 code = operations.lastError();
    const QString message = operations.formatError(code);
    return {code, message.isEmpty() ? unknownHotkeyErrorMessage(code) : message};
}
}

WindowsHotkeyService::WindowsHotkeyService(QObject *parent)
    : WindowsHotkeyService(defaultNativeOperations(), parent) {}

WindowsHotkeyService::WindowsHotkeyService(HotkeyNativeOperations operations, QObject *parent)
    : HotkeyService(parent),
      operations_(std::move(operations)) {
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
}

WindowsHotkeyService::~WindowsHotkeyService() {
    unregisterAll();
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
}

HotkeyRegistrationResult WindowsHotkeyService::registerShortcut(ShortcutAction action,
                                                                 const QKeySequence &sequence) {
    const auto native = toNativeHotkey(sequence);
    if (!native.has_value()) {
        return {false, false, HotkeyError{std::nullopt,
                                          QStringLiteral("Shortcut is empty or unsupported.")}};
    }

    const int id = idForAction(action);
    const auto oldEntry = registrations_.constFind(id);
    if (oldEntry != registrations_.constEnd() && oldEntry->sequence == sequence) {
        return {true, true};
    }

    std::optional<HotkeyEntry> previous;
    if (oldEntry != registrations_.constEnd()) {
        previous = *oldEntry;
        operations_.unregisterHotkey(id);
        registrations_.erase(oldEntry);
    }

    if (!operations_.registerHotkey(id, native->modifiers, native->virtualKey)) {
        HotkeyRegistrationResult result;
        result.error = nativeError(operations_);
        if (previous.has_value()) {
            const auto restoreNative = toNativeHotkey(previous->sequence);
            if (restoreNative.has_value()
                && operations_.registerHotkey(id, restoreNative->modifiers, restoreNative->virtualKey)) {
                registrations_.insert(id, *previous);
                result.previousRestored = true;
            } else if (restoreNative.has_value()) {
                result.recoveryError = nativeError(operations_);
            } else {
                result.recoveryError = HotkeyError{std::nullopt,
                                                    QStringLiteral("Previous shortcut is empty or unsupported.")};
            }
        }
        return result;
    }

    registrations_.insert(id, {action, sequence});
    return {true};
}

QVector<HotkeyActionRegistrationResult> WindowsHotkeyService::registerShortcuts(
    const QVector<HotkeyRegistrationRequest> &requests) {
    QVector<HotkeyActionRegistrationResult> results;
    results.reserve(requests.size());
    for (const HotkeyRegistrationRequest &request : requests) {
        results.append({request.action, request.sequence, {}});
    }

    QVector<bool> changed(requests.size(), false);
    QHash<int, int> changedRequestForAction;
    QHash<QString, int> activeOwnerForSequence;
    for (auto entry = registrations_.cbegin(); entry != registrations_.cend(); ++entry) {
        activeOwnerForSequence.insert(entry->sequence.toString(QKeySequence::PortableText),
                                      entry.key());
    }

    for (qsizetype index = 0; index < requests.size(); ++index) {
        const HotkeyRegistrationRequest &request = requests.at(index);
        const int id = idForAction(request.action);
        const auto existing = registrations_.constFind(id);
        if (!toNativeHotkey(request.sequence).has_value()
            || (existing != registrations_.constEnd() && existing->sequence == request.sequence)) {
            results[index].registration = registerShortcut(request.action, request.sequence);
            continue;
        }
        changed[index] = true;
        changedRequestForAction.insert(id, index);
    }

    QVector<int> dependency(requests.size(), -1);
    for (qsizetype index = 0; index < requests.size(); ++index) {
        if (!changed.at(index)) {
            continue;
        }
        const auto owner = activeOwnerForSequence.constFind(
            requests.at(index).sequence.toString(QKeySequence::PortableText));
        if (owner == activeOwnerForSequence.constEnd()
            || owner.value() == idForAction(requests.at(index).action)) {
            continue;
        }
        const auto ownerRequest = changedRequestForAction.constFind(owner.value());
        if (ownerRequest != changedRequestForAction.constEnd()) {
            dependency[index] = ownerRequest.value();
        }
    }

    QVector<int> visitState(requests.size(), 0);
    QVector<int> visitStack;
    QVector<QVector<int>> cycles;
    std::function<void(int)> findCycles = [&](int index) {
        visitState[index] = 1;
        visitStack.append(index);
        const int next = dependency.at(index);
        if (next >= 0 && changed.at(next)) {
            if (visitState.at(next) == 0) {
                findCycles(next);
            } else if (visitState.at(next) == 1) {
                const qsizetype start = visitStack.indexOf(next);
                QVector<int> cycle;
                for (qsizetype member = start; member < visitStack.size(); ++member) {
                    cycle.append(visitStack.at(member));
                }
                cycles.append(cycle);
            }
        }
        visitStack.removeLast();
        visitState[index] = 2;
    };
    for (qsizetype index = 0; index < requests.size(); ++index) {
        if (changed.at(index) && visitState.at(index) == 0) {
            findCycles(index);
        }
    }

    QVector<bool> processed(requests.size(), false);
    for (qsizetype index = 0; index < requests.size(); ++index) {
        if (!changed.at(index)) {
            processed[index] = true;
        }
    }
    for (const QVector<int> &cycle : cycles) {
        QVector<HotkeyEntry> previous;
        previous.reserve(cycle.size());
        for (int index : cycle) {
            const int id = idForAction(requests.at(index).action);
            const auto entry = registrations_.constFind(id);
            if (entry != registrations_.constEnd()) {
                previous.append(*entry);
                operations_.unregisterHotkey(id);
                registrations_.erase(entry);
            }
        }

        QVector<int> registeredCandidates;
        int failedIndex = -1;
        for (int index : cycle) {
            const auto native = toNativeHotkey(requests.at(index).sequence);
            const int id = idForAction(requests.at(index).action);
            if (!native.has_value() || !operations_.registerHotkey(id, native->modifiers, native->virtualKey)) {
                failedIndex = index;
                results[index].registration.error = native.has_value()
                    ? std::optional<HotkeyError>(nativeError(operations_))
                    : std::optional<HotkeyError>(HotkeyError{std::nullopt,
                        QStringLiteral("Shortcut is empty or unsupported.")});
                break;
            }
            registrations_.insert(id, {requests.at(index).action, requests.at(index).sequence});
            registeredCandidates.append(index);
        }

        if (failedIndex < 0) {
            for (int index : cycle) {
                results[index].registration = {true};
                processed[index] = true;
            }
            continue;
        }

        for (int index : registeredCandidates) {
            const int id = idForAction(requests.at(index).action);
            operations_.unregisterHotkey(id);
            registrations_.remove(id);
        }
        QHash<int, HotkeyError> recoveryErrors;
        QHash<int, bool> restored;
        for (const HotkeyEntry &entry : previous) {
            const int id = idForAction(entry.action);
            const auto native = toNativeHotkey(entry.sequence);
            if (native.has_value() && operations_.registerHotkey(id, native->modifiers, native->virtualKey)) {
                registrations_.insert(id, entry);
                restored.insert(id, true);
            } else {
                recoveryErrors.insert(id, native.has_value()
                    ? nativeError(operations_)
                    : HotkeyError{std::nullopt,
                                   QStringLiteral("Previous shortcut is empty or unsupported.")});
            }
        }
        for (int index : cycle) {
            HotkeyRegistrationResult &result = results[index].registration;
            result.registered = false;
            result.unchanged = false;
            const int id = idForAction(requests.at(index).action);
            if (!result.error.has_value()) {
                result.error = HotkeyError{std::nullopt,
                    QStringLiteral("Shortcut replacement was rolled back because another shortcut in this cycle could not be registered.")};
            }
            result.previousRestored = restored.value(id, false);
            if (recoveryErrors.contains(id)) {
                result.recoveryError = recoveryErrors.value(id);
            }
            processed[index] = true;
        }
    }

    std::function<void(int)> processAcyclic = [&](int index) {
        if (processed.at(index)) {
            return;
        }
        const int owner = dependency.at(index);
        if (owner >= 0) {
            processAcyclic(owner);
            if (!results.at(owner).registration.registered) {
                results[index].registration = {
                    false, false,
                    HotkeyError{std::nullopt,
                        QStringLiteral("Shortcut could not move because the action that owns its requested shortcut did not move.")},
                    true};
                processed[index] = true;
                return;
            }
        }
        results[index].registration = registerShortcut(requests.at(index).action, requests.at(index).sequence);
        processed[index] = true;
    };
    for (qsizetype index = 0; index < requests.size(); ++index) {
        if (changed.at(index)) {
            processAcyclic(index);
        }
    }
    return results;
}

void WindowsHotkeyService::unregisterAll() {
    for (const int id : registrations_.keys()) {
        operations_.unregisterHotkey(id);
    }
    registrations_.clear();
}

bool WindowsHotkeyService::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
    const auto *msg = static_cast<MSG *>(message);
    if (msg && msg->message == WM_HOTKEY) {
        const int id = static_cast<int>(msg->wParam);
        const auto entry = registrations_.constFind(id);
        if (entry != registrations_.constEnd()) {
            emit activated(entry->action);
            if (result != nullptr) {
                *result = TRUE;
            }
            return true;
        }
    }
    return false;
}

std::optional<WindowsHotkeyService::NativeHotkey> WindowsHotkeyService::toNativeHotkey(const QKeySequence &sequence) {
    if (sequence.isEmpty() || sequence.count() > 1) {
        return std::nullopt;
    }

    const QKeyCombination combination = sequence[0];
    const unsigned int virtualKey = toVirtualKey(combination.key());
    if (virtualKey == 0) {
        return std::nullopt;
    }

    unsigned int modifiers = 0;
    const Qt::KeyboardModifiers qtModifiers = combination.keyboardModifiers();
    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        modifiers |= MOD_CONTROL;
    }
    if (qtModifiers.testFlag(Qt::AltModifier)) {
        modifiers |= MOD_ALT;
    }
    if (qtModifiers.testFlag(Qt::ShiftModifier)) {
        modifiers |= MOD_SHIFT;
    }
    if (qtModifiers.testFlag(Qt::MetaModifier)) {
        modifiers |= MOD_WIN;
    }

    return NativeHotkey{modifiers, virtualKey};
}

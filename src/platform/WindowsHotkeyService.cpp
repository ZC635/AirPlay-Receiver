#include "platform/WindowsHotkeyService.h"

#include <QCoreApplication>

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
}

WindowsHotkeyService::WindowsHotkeyService(QObject *parent)
    : HotkeyService(parent) {
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

bool WindowsHotkeyService::registerShortcut(ShortcutAction action, const QKeySequence &sequence) {
    const auto native = toNativeHotkey(sequence);
    if (!native.has_value()) {
        return false;
    }

    const int id = idForAction(action);
    const auto oldEntry = registrations_.constFind(id);
    const bool hadPrevious = (oldEntry != registrations_.constEnd());

    if (hadPrevious) {
        UnregisterHotKey(nullptr, id);
        registrations_.erase(oldEntry);
    }

    if (!RegisterHotKey(nullptr, id, native->modifiers, native->virtualKey)) {
        if (hadPrevious) {
            const auto restoreNative = toNativeHotkey(oldEntry->sequence);
            if (restoreNative.has_value()
                && RegisterHotKey(nullptr, id, restoreNative->modifiers, restoreNative->virtualKey)) {
                registrations_.insert(id, {oldEntry->action, oldEntry->sequence});
            }
        }
        return false;
    }

    registrations_.insert(id, {action, sequence});
    return true;
}

void WindowsHotkeyService::unregisterAll() {
    for (const int id : registrations_.keys()) {
        UnregisterHotKey(nullptr, id);
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

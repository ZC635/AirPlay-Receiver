#include "platform/WindowsHotkeyService.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
    if (key == Qt::Key_Up) {
        return 0x26;
    }
    if (key == Qt::Key_Down) {
        return 0x28;
    }
    return 0;
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

#ifdef Q_OS_WIN
    const int id = idForAction(action);
    if (actionsById.contains(id)) {
        UnregisterHotKey(nullptr, id);
        actionsById.remove(id);
    }

    if (!RegisterHotKey(nullptr, id, native->modifiers, native->virtualKey)) {
        return false;
    }

    actionsById.insert(id, action);
    return true;
#else
    return false;
#endif
}

void WindowsHotkeyService::unregisterAll() {
#ifdef Q_OS_WIN
    for (const int id : actionsById.keys()) {
        UnregisterHotKey(nullptr, id);
    }
#endif
    actionsById.clear();
}

bool WindowsHotkeyService::nativeEventFilter(const QByteArray &, void *message, qintptr *) {
#ifdef Q_OS_WIN
    const auto *msg = static_cast<MSG *>(message);
    if (msg && msg->message == WM_HOTKEY) {
        const int id = static_cast<int>(msg->wParam);
        const auto action = actionsById.constFind(id);
        if (action != actionsById.constEnd()) {
            emit activated(action.value());
            return true;
        }
    }
#else
    Q_UNUSED(message);
#endif
    return false;
}

std::optional<WindowsHotkeyService::NativeHotkey> WindowsHotkeyService::toNativeHotkey(const QKeySequence &sequence) {
    if (sequence.isEmpty()) {
        return std::nullopt;
    }

    const QKeyCombination combination = sequence[0];
    const unsigned int virtualKey = toVirtualKey(combination.key());
    if (virtualKey == 0) {
        return std::nullopt;
    }

    unsigned int modifiers = 0;
    const Qt::KeyboardModifiers qtModifiers = combination.keyboardModifiers();
#ifdef Q_OS_WIN
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
#else
    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        modifiers |= 0x0002;
    }
    if (qtModifiers.testFlag(Qt::AltModifier)) {
        modifiers |= 0x0001;
    }
    if (qtModifiers.testFlag(Qt::ShiftModifier)) {
        modifiers |= 0x0004;
    }
    if (qtModifiers.testFlag(Qt::MetaModifier)) {
        modifiers |= 0x0008;
    }
#endif

    return NativeHotkey{modifiers, virtualKey};
}

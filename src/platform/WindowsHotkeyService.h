#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>

#include <optional>

#include "platform/HotkeyService.h"

class WindowsHotkeyService : public HotkeyService, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    struct NativeHotkey {
        unsigned int modifiers;
        unsigned int virtualKey;
    };

    explicit WindowsHotkeyService(QObject *parent = nullptr);
    ~WindowsHotkeyService() override;

    bool registerShortcut(ShortcutAction action, const QKeySequence &sequence) override;
    void unregisterAll() override;
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

    static std::optional<NativeHotkey> toNativeHotkey(const QKeySequence &sequence);

private:
    QHash<int, ShortcutAction> actionsById;
};

#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>

#include <functional>
#include <optional>

#include "platform/HotkeyService.h"

struct HotkeyNativeOperations {
    std::function<bool(int, unsigned int, unsigned int)> registerHotkey;
    std::function<void(int)> unregisterHotkey;
    std::function<quint32()> lastError;
    std::function<QString(quint32)> formatError;
};

class WindowsHotkeyService : public HotkeyService, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    struct NativeHotkey {
        unsigned int modifiers;
        unsigned int virtualKey;
    };

    explicit WindowsHotkeyService(QObject *parent = nullptr);
    WindowsHotkeyService(HotkeyNativeOperations operations, QObject *parent = nullptr);
    ~WindowsHotkeyService() override;

    HotkeyRegistrationResult registerShortcut(ShortcutAction action,
                                              const QKeySequence &sequence) override;
    void unregisterAll() override;
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

    static std::optional<NativeHotkey> toNativeHotkey(const QKeySequence &sequence);

private:
    struct HotkeyEntry {
        ShortcutAction action;
        QKeySequence sequence;
    };
    QHash<int, HotkeyEntry> registrations_;
    HotkeyNativeOperations operations_;
    friend class HotkeyServiceTest;
};

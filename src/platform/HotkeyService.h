#pragma once

#include <QKeySequence>
#include <QObject>

#include <optional>

#include "app/ShortcutAction.h"

struct HotkeyError {
    std::optional<quint32> nativeCode;
    QString message;
};

struct HotkeyRegistrationResult {
    bool registered = false;
    bool unchanged = false;
    std::optional<HotkeyError> error;
    bool previousRestored = false;
    std::optional<HotkeyError> recoveryError;
};

class HotkeyService : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    virtual HotkeyRegistrationResult registerShortcut(ShortcutAction action,
                                                      const QKeySequence &sequence) = 0;
    virtual void unregisterAll() = 0;

signals:
    void activated(ShortcutAction action);
};

#pragma once

#include <QKeySequence>
#include <QObject>
#include <QVector>

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

struct HotkeyRegistrationRequest {
    ShortcutAction action;
    QKeySequence sequence;
};

struct HotkeyActionRegistrationResult {
    ShortcutAction action;
    QKeySequence attemptedSequence;
    HotkeyRegistrationResult registration;
};

class HotkeyService : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    virtual HotkeyRegistrationResult registerShortcut(ShortcutAction action,
                                                      const QKeySequence &sequence) = 0;
    virtual QVector<HotkeyActionRegistrationResult> registerShortcuts(
        const QVector<HotkeyRegistrationRequest> &requests) {
        QVector<HotkeyActionRegistrationResult> results;
        results.reserve(requests.size());
        for (const HotkeyRegistrationRequest &request : requests) {
            results.append({request.action, request.sequence,
                            registerShortcut(request.action, request.sequence)});
        }
        return results;
    }
    virtual void unregisterAll() = 0;

signals:
    void activated(ShortcutAction action);
};
